/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <config.h>

#include "Util.hpp"

#include <csignal>
#include <sys/poll.h>
#ifdef __linux
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#elif defined IOS
#import <Foundation/Foundation.h>
#endif
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include <Poco/Base64Encoder.h>
#include <Poco/HexBinaryEncoder.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/Exception.h>
#include <Poco/Format.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Process.h>
#include <Poco/RandomStream.h>
#include <Poco/TemporaryFile.h>
#include <Poco/Thread.h>
#include <Poco/Timestamp.h>
#include <Poco/Util/Application.h>

#include "Common.hpp"
#include "Log.hpp"
#include "Util.hpp"

using std::size_t;

namespace Util
{
    namespace rng
    {
        static std::random_device _rd;
        static std::mutex _rngMutex;
        static Poco::RandomBuf _randBuf;

        // Create the prng with a random-device for seed.
        // If we don't have a hardware random-device, we will get the same seed.
        // In that case we are better off with an arbitrary, but changing, seed.
        static std::mt19937_64 _rng = std::mt19937_64(_rd.entropy()
                                                    ? _rd()
                                                    : (clock() + getpid()));

        // A new seed is used to shuffle the sequence.
        // N.B. Always reseed after getting forked!
        void reseed()
        {
            _rng.seed(_rd.entropy() ? _rd() : (clock() + getpid()));
        }

        // Returns a new random number.
        unsigned getNext()
        {
            std::unique_lock<std::mutex> lock(_rngMutex);
            return _rng();
        }

        std::vector<char> getBytes(const size_t length)
        {
            std::vector<char> v(length);
            _randBuf.readFromDevice(v.data(), v.size());
            return v;
        }

        /// Generate a string of random characters.
        std::string getHexString(const size_t length)
        {
            std::stringstream ss;
            Poco::HexBinaryEncoder hex(ss);
            hex.write(getBytes(length).data(), length);
            return ss.str().substr(0, length);
        }

        /// Generates a random string in Base64.
        /// Note: May contain '/' characters.
        std::string getB64String(const size_t length)
        {
            std::stringstream ss;
            Poco::Base64Encoder b64(ss);
            b64.write(getBytes(length).data(), length);
            return ss.str().substr(0, length);
        }

        std::string getFilename(const size_t length)
        {
            std::string s = getB64String(length * 2);
            s.erase(std::remove_if(s.begin(), s.end(),
                                   [](const std::string::value_type& c)
                                   {
                                       // Remove undesirable characters in a filename.
                                       return c == '/' || c == ' ' || c == '+';
                                   }),
                     s.end());
            return s.substr(0, length);
        }
    }

    static std::string getDefaultTmpDir()
    {
        const char *tmp = getenv("TMPDIR");
        if (!tmp)
            tmp = getenv("TEMP");
        if (!tmp)
            tmp = getenv("TMP");
        if (!tmp)
            tmp = "/tmp";
        return tmp;
    }

    std::string createRandomTmpDir()
    {
        std::string defaultTmp = getDefaultTmpDir();
        std::string newTmp =
            defaultTmp + "/lool-" + rng::getFilename(16);
        if (::mkdir(newTmp.c_str(), S_IRWXU) < 0) {
            LOG_ERR("Failed to create random temp directory");
            return defaultTmp;
        }
        return newTmp;
    }

#if !MOBILEAPP
    int getProcessThreadCount()
    {
        DIR *fdDir = opendir("/proc/self/task");
        if (!fdDir)
        {
            LOG_ERR("No proc mounted");
            return -1;
        }
        int tasks = 0;
        while (readdir(fdDir))
            tasks++;
        closedir(fdDir);
        return tasks;
    }

    // close what we have - far faster than going up to a 1m open_max eg.
    static bool closeFdsFromProc()
    {
          DIR *fdDir = opendir("/proc/self/fd");
          if (!fdDir)
              return false;

          struct dirent *i;

          while ((i = readdir(fdDir))) {
              if (i->d_name[0] == '.')
                  continue;

              char *e = NULL;
              errno = 0;
              long fd = strtol(i->d_name, &e, 10);
              if (errno != 0 || !e || *e)
                  continue;

              if (fd == dirfd(fdDir))
                  continue;

              if (fd < 3)
                  continue;

              if (close(fd) < 0)
                  std::cerr << "Unexpected failure to close fd " << fd << std::endl;
          }

          closedir(fdDir);
          return true;
    }

    static void closeFds()
    {
        if (!closeFdsFromProc())
        {
            std::cerr << "Couldn't close fds efficiently from /proc" << std::endl;
            for (int fd = 3; fd < sysconf(_SC_OPEN_MAX); ++fd)
                close(fd);
        }
    }

    int spawnProcess(const std::string &cmd, const std::vector<std::string> &args, int *stdInput)
    {
        int pipeFds[2] = { -1, -1 };
        if (stdInput)
        {
            if (pipe(pipeFds) < 0)
            {
                LOG_ERR("Out of file descriptors spawning " << cmd);
                throw Poco::SystemException("Out of file descriptors");
            }
        }

        std::vector<char *> params;
        params.push_back(const_cast<char *>(cmd.c_str()));
        for (const auto& i : args)
            params.push_back(const_cast<char *>(i.c_str()));
        params.push_back(nullptr);

        int pid = fork();
        if (pid < 0)
        {
            LOG_ERR("Failed to fork for command '" << cmd);
            throw Poco::SystemException("Failed to fork for command ", cmd);
        }
        else if (pid == 0) // child
        {
            if (stdInput)
                dup2(pipeFds[0], STDIN_FILENO);

            closeFds();

            int ret = execvp(params[0], &params[0]);
            if (ret < 0)
                std::cerr << "Failed to exec command '" << cmd << "' with error '" << strerror(errno) << "'\n";
            Log::shutdown();
            _exit(42);
        }
        // else spawning process still
        if (stdInput)
        {
            close(pipeFds[0]);
            *stdInput = pipeFds[1];
        }
        return pid;
    }
#endif

    bool dataFromHexString(const std::string& hexString, std::vector<unsigned char>& data)
    {
        if (hexString.length() % 2 != 0)
        {
            return false;
        }

        data.clear();
        std::stringstream stream;
        unsigned value;
        for (unsigned long offset = 0; offset < hexString.size(); offset += 2)
        {
            stream.clear();
            stream << std::hex << hexString.substr(offset, 2);
            stream >> value;
            data.push_back(static_cast<unsigned char>(value));
        }

        return true;
    }

    std::string encodeId(const unsigned number, const int padding)
    {
        std::ostringstream oss;
        oss << std::hex << std::setw(padding) << std::setfill('0') << number;
        return oss.str();
    }

    unsigned decodeId(const std::string& str)
    {
        unsigned id = 0;
        std::stringstream ss;
        ss << std::hex << str;
        ss >> id;
        return id;
    }

    bool windowingAvailable()
    {
        return std::getenv("DISPLAY") != nullptr;
    }

#if !MOBILEAPP

    static const char *startsWith(const char *line, const char *tag)
    {
        size_t len = std::strlen(tag);
        if (!strncmp(line, tag, len))
        {
            while (!isdigit(line[len]) && line[len] != '\0')
                ++len;

            return line + len;
        }

        return nullptr;
    }

    std::string getHumanizedBytes(unsigned long nBytes)
    {
        constexpr unsigned factor = 1024;
        short count = 0;
        float val = nBytes;
        while (val >= factor && count < 4) {
            val /= factor;
            count++;
        }
        std::string unit;
        switch (count)
        {
        case 0: unit = ""; break;
        case 1: unit = "ki"; break;
        case 2: unit = "Mi"; break;
        case 3: unit = "Gi"; break;
        case 4: unit = "Ti"; break;
        default: assert(false);
        }

        unit += "B";
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1) << val << ' ' << unit;
        return ss.str();
    }

    size_t getTotalSystemMemoryKb()
    {
        size_t totalMemKb = 0;
        FILE* file = fopen("/proc/meminfo", "r");
        if (file != nullptr)
        {
            char line[4096] = { 0 };
            while (fgets(line, sizeof(line), file))
            {
                const char* value;
                if ((value = startsWith(line, "MemTotal:")))
                {
                    totalMemKb = atoi(value);
                    break;
                }
            }
        }

        return totalMemKb;
    }

    std::pair<size_t, size_t> getPssAndDirtyFromSMaps(FILE* file)
    {
        size_t numPSSKb = 0;
        size_t numDirtyKb = 0;
        if (file)
        {
            rewind(file);
            char line[4096] = { 0 };
            while (fgets(line, sizeof (line), file))
            {
                const char *value;
                // Shared_Dirty is accounted for by forkit's RSS
                if ((value = startsWith(line, "Private_Dirty:")))
                {
                    numDirtyKb += atoi(value);
                }
                else if ((value = startsWith(line, "Pss:")))
                {
                    numPSSKb += atoi(value);
                }
            }
        }

        return std::make_pair(numPSSKb, numDirtyKb);
    }

    std::string getMemoryStats(FILE* file)
    {
        const std::pair<size_t, size_t> pssAndDirtyKb = getPssAndDirtyFromSMaps(file);
        std::ostringstream oss;
        oss << "procmemstats: pid=" << getpid()
            << " pss=" << pssAndDirtyKb.first
            << " dirty=" << pssAndDirtyKb.second;
        LOG_TRC("Collected " << oss.str());
        return oss.str();
    }

    size_t getMemoryUsagePSS(const Poco::Process::PID pid)
    {
        if (pid > 0)
        {
            const auto cmd = "/proc/" + std::to_string(pid) + "/smaps";
            FILE* fp = fopen(cmd.c_str(), "r");
            if (fp != nullptr)
            {
                const size_t pss = getPssAndDirtyFromSMaps(fp).first;
                fclose(fp);
                return pss;
            }
        }

        return 0;
    }

    size_t getMemoryUsageRSS(const Poco::Process::PID pid)
    {
        static const int pageSizeBytes = getpagesize();
        size_t rss = 0;

        if (pid > 0)
        {
            rss = getStatFromPid(pid, 23);
            rss *= pageSizeBytes;
            rss /= 1024;
            return rss;
        }
        return 0;
    }

    size_t getCpuUsage(const Poco::Process::PID pid)
    {
        if (pid > 0)
        {
            size_t totalJiffies = 0;
            totalJiffies += getStatFromPid(pid, 13);
            totalJiffies += getStatFromPid(pid, 14);
            return totalJiffies;
        }
        return 0;
    }

    size_t getStatFromPid(const Poco::Process::PID pid, int ind)
    {
        if (pid > 0)
        {
            const auto cmd = "/proc/" + std::to_string(pid) + "/stat";
            FILE* fp = fopen(cmd.c_str(), "r");
            if (fp != nullptr)
            {
                char line[4096] = { 0 };
                if (fgets(line, sizeof (line), fp))
                {
                    const std::string s(line);
                    int index = 1;
                    size_t pos = s.find(' ');
                    while (pos != std::string::npos)
                    {
                        if (index == ind)
                        {
                            fclose(fp);
                            return strtol(&s[pos], nullptr, 10);
                        }
                        ++index;
                        pos = s.find(' ', pos + 1);
                    }
                }
            }
        }
        return 0;
    }
#endif

    std::string replace(std::string result, const std::string& a, const std::string& b)
    {
        const size_t aSize = a.size();
        if (aSize > 0)
        {
            const size_t bSize = b.size();
            std::string::size_type pos = 0;
            while ((pos = result.find(a, pos)) != std::string::npos)
            {
                result = result.replace(pos, aSize, b);
                pos += bSize; // Skip the replacee to avoid endless recursion.
            }
        }

        return result;
    }

    std::string formatLinesForLog(const std::string& s)
    {
        std::string r;
        std::string::size_type n = s.size();
        if (n > 0 && s.back() == '\n')
            r = s.substr(0, n-1);
        else
            r = s;
        return replace(r, "\n", " / ");
    }

    static thread_local char ThreadName[32] = {0};

    void setThreadName(const std::string& s)
    {
        strncpy(ThreadName, s.c_str(), 31);
        ThreadName[31] = '\0';
#ifdef __linux
        if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(s.c_str()), 0, 0, 0) != 0)
            LOG_SYS("Cannot set thread name of " << getThreadId() << " (" << std::hex <<
                    std::this_thread::get_id() << std::dec << ") to [" << s << "].");
        else
            LOG_INF("Thread " << getThreadId() << " (" << std::hex <<
                    std::this_thread::get_id() << std::dec << ") is now called [" << s << "].");
#elif defined IOS
        [[NSThread currentThread] setName:[NSString stringWithUTF8String:ThreadName]];
        LOG_INF("Thread " << getThreadId() <<
                ") is now called [" << s << "].");
#endif
    }

    const char *getThreadName()
    {
        // Main process and/or not set yet.
        if (ThreadName[0] == '\0')
        {
#ifdef __linux
            if (prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(ThreadName), 0, 0, 0) != 0)
                strncpy(ThreadName, "<noid>", sizeof(ThreadName) - 1);
#elif defined IOS
            const char *const name = [[[NSThread currentThread] name] UTF8String];
            strncpy(ThreadName, name, 31);
            ThreadName[31] = '\0';
#endif
        }

        // Avoid so many redundant system calls
        return ThreadName;
    }

#ifdef __linux
    static thread_local pid_t ThreadTid = 0;

    pid_t getThreadId()
#else
    std::thread::id getThreadId()
#endif
    {
        // Avoid so many redundant system calls
#ifdef __linux
        if (!ThreadTid)
            ThreadTid = ::syscall(SYS_gettid);
        return ThreadTid;
#else
        return std::this_thread::get_id();
#endif
    }

    void getVersionInfo(std::string& version, std::string& hash)
    {
        version = std::string(LOOLWSD_VERSION);
        hash = std::string(LOOLWSD_VERSION_HASH);
        hash.resize(std::min(8, (int)hash.length()));
    }

    std::string UniqueId()
    {
        static std::atomic_int counter(0);
        return std::to_string(Poco::Process::id()) + '/' + std::to_string(counter++);
    }

    std::map<std::string, std::string> JsonToMap(const std::string& jsonString)
    {
        std::map<std::string, std::string> map;
        if (jsonString.empty())
            return map;

        Poco::JSON::Parser parser;
        const Poco::Dynamic::Var result = parser.parse(jsonString);
        const auto& json = result.extract<Poco::JSON::Object::Ptr>();

        std::vector<std::string> names;
        json->getNames(names);

        for (const auto& name : names)
        {
            map[name] = json->get(name).toString();
        }

        return map;
    }

    bool isValidURIScheme(const std::string& scheme)
    {
        if (scheme.empty())
            return false;

        for (char c : scheme)
        {
            if (!isalpha(c))
                return false;
        }

        return true;
    }

    bool isValidURIHost(const std::string& host)
    {
        if (host.empty())
            return false;

        for (char c : host)
        {
            if (!isalnum(c) && c != '_' && c != '-' && c != '.' && c !=':' && c != '[' && c != ']')
                return false;
        }

        return true;
    }

    /// Split a string in two at the delimeter and give the delimiter to the first.
    static
    std::pair<std::string, std::string> splitLast2(const char* s, const int length, const char delimeter = ' ')
    {
        if (s != nullptr && length > 0)
        {
            const int pos = getLastDelimiterPosition(s, length, delimeter);
            if (pos < length)
                return std::make_pair(std::string(s, pos + 1), std::string(s + pos + 1));
        }

        // Not found; return in first.
        return std::make_pair(std::string(s, length), std::string());
    }

    std::tuple<std::string, std::string, std::string, std::string> splitUrl(const std::string& url)
    {
        // In case we have a URL that has parameters.
        std::string base;
        std::string params;
        std::tie(base, params) = Util::split(url, '?', false);

        std::string filename;
        std::tie(base, filename) = Util::splitLast2(base.c_str(), base.size(), '/');
        if (filename.empty())
        {
            // If no '/', then it's only filename.
            std::swap(base, filename);
        }

        std::string ext;
        std::tie(filename, ext) = Util::splitLast(filename, '.', false);

        return std::make_tuple(base, filename, ext, params);
    }

    static std::map<std::string, std::string> AnonymizedStrings;
    static std::atomic<unsigned> AnonymizationSalt(0);
    static std::mutex AnonymizedMutex;

    void mapAnonymized(const std::string& plain, const std::string& anonymized)
    {
        if (plain.empty() || anonymized.empty())
            return;

        auto &log = Log::logger();
        if (log.trace() && plain != anonymized)
            LOG_TRC("Anonymizing [" << plain << "] -> [" << anonymized << "].");

        std::unique_lock<std::mutex> lock(AnonymizedMutex);

        AnonymizedStrings[plain] = anonymized;
    }

    std::string anonymize(const std::string& text)
    {
        {
            std::unique_lock<std::mutex> lock(AnonymizedMutex);

            const auto it = AnonymizedStrings.find(text);
            if (it != AnonymizedStrings.end())
            {
                auto &log = Log::logger();
                if (log.trace() && text != it->second)
                    LOG_TRC("Found anonymized [" << text << "] -> [" << it->second << "].");
                return it->second;
            }
        }

        // We just need something irreversible, short, and
        // quite simple.
        std::size_t hash = 0;
        for (const char c : text)
            hash += c;

        // Generate the anonymized string. The '#' is to hint that it's anonymized.
        // Prepend with salt to make it unique, in case we get collisions (which we will, eventually).
        const std::string res = '#' + Util::encodeId(AnonymizationSalt++, 0) + '#' + Util::encodeId(hash, 0) + '#';
        mapAnonymized(text, res);
        return res;
    }

    std::string getFilenameFromURL(const std::string& url)
    {
        std::string base;
        std::string filename;
        std::string ext;
        std::string params;
        std::tie(base, filename, ext, params) = Util::splitUrl(url);
        return filename;
    }

    std::string anonymizeUrl(const std::string& url)
    {
        std::string base;
        std::string filename;
        std::string ext;
        std::string params;
        std::tie(base, filename, ext, params) = Util::splitUrl(url);

        return base + Util::anonymize(filename) + ext + params;
    }

    std::string getHttpTimeNow()
    {
        char time_now[50];
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm = *std::gmtime(&now_c);
        strftime(time_now, 50, "%a, %d %b %Y %T", &now_tm);

        return time_now;
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
