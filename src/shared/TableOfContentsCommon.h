/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// --- shared by TableOfContentsCommon.cpp and each app's TableOfContents.cpp ---

IPageDestination* SnapshotDestForDeferredNav(IPageDestination* dest, int tocPageNo);
TocItem* FindTocItemByTitlePage(TocItem* item, Str title, int pageNo);
struct GoToTocLinkData {
    WindowTab* tab = nullptr;
    DocController* ctrl = nullptr;
    // owned snapshot; may be null (then pageNo alone is used)
    IPageDestination* dest = nullptr;
    int pageNo = 0;
    // owned; used to re-select in tree after palette-driven nav
    Str title;
    // true when the navigation was driven from outside the tree (e.g. the
    // command palette), so afterwards we must move the tree's selection to the
    // item ourselves. For tree-driven navigation the tree is already selected.
    bool selectInTree = false;

    ~GoToTocLinkData() {
        delete dest;
        str::Free(title);
    }
};
GoToTocLinkData* NewGoToTocLinkData(MainWindow* win, TocItem* tocItem, bool selectInTree);
bool IsScrollToLink(IPageDestination* link);
void CollectTocItemsOnPage(TreeModel* tm, int pageNo, Vec<TocItem*>& out);
bool TocMatchingItemsContains(const Vec<TocItem*>& items, TocItem* item);
bool TocMatchingItemsEq(const Vec<TocItem*>& a, const Vec<TocItem*>& b);
void GetLeftRightCounts(TocItem* node, int& l2r, int& r2l);
void SetInitialExpandState(TocItem* item, Vec<int>& tocState);
void AddFavoriteFromToc(MainWindow* win, TocItem* dti);
void AutoExpandTopLevelItems(TocItem* root);
TocItem* FindTocItemByTitleAndPage(TocItem* item, Str title, int pageNo);
TocItem* FilterTocItemRec(TocItem* item, const StrVec& words);

// implemented by each app
void GoToTocLink(GoToTocLinkData* d);

TocItem* TreeItemForPageNo(TreeModel* tm, int pageNo);
TocItem* TreeItemForChapter(TreeModel* tm, int chapter, int pageNo);
void OpenAttachment(WindowTab* tab, Str fileName, int attachmentNo);

void GoToTocTreeItem(MainWindow* win, TocItem* tocItem, bool allowExternal);

// --- bookmarks as plain text, for import / export ---
//
// One entry per line: leading tabs give the nesting, then the name in double
// quotes, then the page number.
//
//   "Chapter 1" 1
//   \t"Section 1.1" 12
//   \t\t"How \"quoted\" works" 13
//
//   <depth tabs> "<name>" <whitespace> <page>
//
// Inside the name, \" is a double quote, \\ a backslash and \n \r \t the
// control characters. Blank lines and lines whose first non-tab character is
// # are ignored. Page numbers are 1-based; 0 marks an entry that has no
// destination (an outline entry can carry a name and nothing else).

// Serialize `first` (the first of a sibling list) and its children. Entries
// that came from the document's embedded files are left out: they are not
// bookmarks of the document. Returns a heap string the caller must
// str::Free().
Str BookmarkTextFromToc(TocItem* first);

// Parse bookmarks text into a TocItem tree allocated with AllocTocItem(),
// i.e. on the heap: release it with FreeTocItemRec(nullptr, root). Returns ""
// when the whole text was read, otherwise a message naming the line that went
// wrong; *rootInOut is then left untouched.
TempStr BookmarkTextParse(Str text, TocItem** rootInOut);
