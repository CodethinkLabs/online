
LibreOffice Online API
======================

Document conversion
-------------------

 **API:** HTTP POST to `/lool/convert-to/<format>`
  * the format is e.g. "png", "pdf" or "txt"
  * the file itself in the payload
### Example:

    curl -F "data=@test.txt" https://localhost:9980/lool/convert-to/docx > out.docx

  * or in HTML:
```html
<form action="https://localhost:9980/lool/convert-to/docx" enctype="multipart/form-data" method="post">
    File: <input type="file" name="data"><br/>
    <input type="submit" value="Convert to DOCX">
</form>
```

Alternatively you can omit the `<format>`, and instead provide it as another parameter
### Example:

    curl -F "data=@test.odt" -F "format=pdf" https://localhost:9980/lool/convert-to > out.pdf

* or in HTML:
```html
<form action="https://localhost:9980/lool/convert-to" enctype="multipart/form-data" method="post">
    File: <input type="file" name="data"><br/>
    Format: <input type="text" name="format"><br/>
    <input type="submit" value="Convert">
</form>
```

WOPI Extensions
===============

LibreOffice Online uses a WOPI-like protocol to interact with hosts who want to integrate LibreOffice Online in them.

Refer to [WOPI docs](https://wopi.readthedocs.io/en/latest/) for further details on the protocol's inspiration.

CheckFileInfo response properties
---------------------------------

### BaseFileName
A string containing the basename of the file, omitting its path.

### DisablePrint
Disables print functionality in libreoffice online backend. Ift rue, HidePrintOption is assumed to be true

### OwnerID
A programmatic string identifier for the owner of the file.

### PostMessageOrigin
A string for the domain the host page sends/receives PostMessages from, we only listen to messages from this domain.

### Size
Size of the file in bytes (64bit)

### TemplateSaveAs
In case this file should be treated as a template, the file name (potentially including a suitable path - that the WOPI host has to interpret) will be used as the real name under which the resulting file will be stored.

Storing the file resulting from a template uses the normal PutRelativeFile workflow, which means a new CheckFileInfo will have to be sent upon load of the resulting file.

### UserCanWrite
A boolean flag, indicating whether the user has permission to edit and/or over-write the file. If not set PutFile will fail.

### UserCanNotWriteRelative
A boolean flag indiciating that the user cannot Save-As on this server, so PutFileRelative will fail.

### UserId
A programmatic string identifier of the user.

### UserFriendlyName
A string representing the name of the user for display in the UI.


CheckFileInfo extended response properties
------------------------------------------

### HidePrintOption
If set to true, hides the print option from the filemenu bar in the UI

### HideSaveOption
If set to true, hides the save button from the toolbar and file menubar	in the UI

### HideExportOption
Hides 'Download as' option in the file menubar

### DisableExport
Disables export functionality in backend. If set to true, HideExportOption is assumed to be true

### DisableCopy
Disables copying from the document in libreoffice online backend. Pasting into the document would still be possible. However, it is still possible to do an "internal" cut/copy/paste.

### DisableInactiveMessages
Disables displaying of the explanation text on the overlay when the document becomes inactive or killed.  With this, the JS integration must provide the user with appropriate message when it gets Session_Closed or User_Idle postMessage's.

### DownloadAsPostMessage
Indicate that the integration wants to handle the downloading of pdf for printing or svg for slideshows or experted document, because it cannot rely on browser's support for downloading.

When this is set to true, the user's eg. Print action will trigger a postMessage called Download_As, with the following JSON in the Values:

    { Type: 'print'|'slideshow'|'export', URL: ...url you use for the actual downloading... }

### EnableOwnerTermination
If set to true, it allows the document owner (the one with OwnerId =UserId) to send a 'closedocument' message (see protocol.txt)

### UserExtraInfo
JSON object that contains additional info about the user, namely the avatar image.

**Example:** 'UserExtraInfo' => [ 'avatar' => 'http://url/to/user/avatar', 'mail' => 'user@server.com' ]

**Note:** There is strict Content Security Policy that restricts image resources (img-src), therefore the avatar URL must not violate the	CSP,  otherwise it will show as broken images.

### WatermarkText
If set to a non-empty string, is used for rendering a watermark-like text on each tile of the document

**Note:** It is possible to just hide print, save, export options while still being able to access them from other hosts using PostMessage API (see [loleaflet/reference.html](https://www.collaboraoffice.com/collabora-online-editor-api-reference/))

PostMessage extensions
--------------------------------------

### App_LoadingStatus
Was extended with field 'Status' with 'Document_Loaded' value when document was loaded successfully and 'Failed' in other case.

Alternative authentication possibility
--------------------------------------

Instead of the 'access_token', it is possible to pass an 'access_header' at the places where the 'access_token' would be used in the initial iframe setup.

The 'access_header' can be eg. of a form

    Authorization: Basic abcd1234==

This header is then used in all the protocol calls like PutFile, GetFile or CheckFileInfo, allowing Basic authentication to work.

PutFile headers
---------------

PutFile additionally indicates whether the user has modified the document before the save, or if they just pressed the Save button without any modification.  The following header:

    X-LOOL-WOPI-IsModifiedByUser

will have the value 'true' or 'false' accordingly.

To distinguish autosave vs. explicit user requests to save, the following header:

    X-LOOL-WOPI-IsAutosave

will have the value 'true' when the PutFile is triggered by autosave, and 'false' when triggered by explicit user operation (Save button or menu entry).

When the document gets cleaned up from memory (e.g. when all users disconnect), an automatic save will be triggered. In this case the following header will be set to "true":

    X-LOOL-WOPI-IsExitSave

Detecting external document change
----------------------------------

Locking is omitted from our WOPI-like protocol since it goes against common EFSS solutions usage. Instead, LibreOffice Online uses timestamps to detect document changes.

When the document is updated in your storage while being edited in LibreOffice Online and there are unsaved changes, we detect it as soon as possible and ask the user if he/she would like to overwrite the changes or reload the new document from the storage.

In case there are no unsaved changes, we reload the new document without asking the user.

To support this feature, the host implementation has to specify LastModifiedTime field in both CheckFileInfo and PutFile calls.

Additionally, hosts must check for a header in PutFile response:

    X-LOOL-WOPI-Timestamp

This header contains the ISO8601 round-trip formatted time of file’s last modified time in storage, as known by LibreOffice Online. In case this header is present and its value does not match the file’s modified time in storage, it indicates that document being edited is not the one that is present in the storage.

Hosts should not save the file to storage in such cases and respond with HTTP 409 along with LibreOffice Online specific status code:

    HTTP 409 with JSON:
    {
        “LOOLStatusCode”: 1010
    }

When the user chooses "overwrite" when asked how to resolve the conflict, LibreOffice will attempt one more save operation, but this time it will lack the X-LOOL-WOPI-Timestamp header, which means "save regardless of state of the
file".

/hosting/capabilities
---------------------

With new features, it is important for the integrations to know if the Online they are using is supporting them.  For this reason, we have introduced a /hosting/capabilities endpoint that returns a JSON with information about the availability of various features.

Currently the following are present:

* convert-to: {available: true/false }

  The property *available* is *true* when the convert-to functionality is present and correctly accessible from the WOPI host.

* hasTemplateSaveAs: true/false

  is *true* when the Online supports the TemplateSaveAs CheckFileInfo property.

* hasMobileSupport: true/false

  is *true* when the Online has a good support for the mobile devices and responsive design.
