/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

bool HasKnownExternalViewerForCmd(int cmd);

void DetectExternalViewers();
void FreeExternalViewers();
bool CanViewWithKnownExternalViewer(WindowTab* tab, int cmd);
bool ViewWithKnownExternalViewer(WindowTab* tab, int cmd);

bool CanSendAsEmailAttachment(WindowTab* tab = nullptr);
bool SendAsEmailAttachment(WindowTab* tab, HWND hwndParent = nullptr);

bool CouldBePDFDoc(WindowTab*);
bool PathMatchFilter(const char* path, char* filter);

bool IsCustomExternalViewerCmdId(int cmdId);
bool ViewWithCustomExternalViewer(WindowTab* tab, int idx);

void CreateExternalViewersCommands();
