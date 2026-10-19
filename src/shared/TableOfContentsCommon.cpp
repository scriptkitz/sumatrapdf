/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/UITask.h"
#include "gui/UIModels.h"
#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "DisplayModel.h"
#include "Favorites.h"
#include "WindowTab.h"
#include "Commands.h"
#include "Translations.h"
#include "Tabs.h"
#include "Menu.h"
#include "Accelerators.h"
#include "Theme.h"
#include "FilterUtil.h"
#include "TableOfContents.h"
#include "TableOfContentsCommon.h"

// When true, multi-highlight every TOC item that matches the current page
// (issue #4642). Easy to flip for comparison with single-selection behavior.
bool gShowAllMatchingTOC = true;

// Own a stable copy of the destination at post time. Engine-private kinds
// that hold fz_outline/fz_link (mupdf) are converted to scrollTo with
// page/rect/zoom already resolved on the TocItem/dest.
IPageDestination* SnapshotDestForDeferredNav(IPageDestination* dest, int tocPageNo) {
    if (!dest) {
        return nullptr;
    }
    Kind k = dest->GetKind();
    if (k == kindDestinationLaunchURL) {
        Str url = ((PageDestinationURL*)dest)->url;
        if (len(url) == 0) {
            url = dest->GetValue();
        }
        return url ? new PageDestinationURL(url) : nullptr;
    }
    if (k == kindDestinationLaunchFile) {
        auto* f = (PageDestinationFile*)dest;
        auto* copy = new PageDestinationFile(f->path, f->dest);
        copy->openInNewWindow = f->openInNewWindow;
        copy->rect = f->rect;
        return copy;
    }
    if (k == kindDestinationLaunchEmbedded || k == kindDestinationAttachment) {
        auto* p = (PageDestination*)dest;
        auto* copy = new PageDestination();
        copy->kind = k;
        copy->pageNo = p->pageNo;
        copy->rect = p->rect;
        copy->zoom = p->zoom;
        copy->value = str::Dup(p->value);
        copy->name = str::Dup(p->name);
        copy->embedObjNum = p->embedObjNum;
        return copy;
    }
    if (k == kindDestinationScrollTo) {
        int pageNo = PageDestGetPageNo(dest);
        if (pageNo <= 0) {
            pageNo = tocPageNo;
        }
        if (pageNo < 1) {
            logf("SnapshotDestForDeferredNav: skip scrollTo pageNo=%d (tocPageNo=%d)\n", PageDestGetPageNo(dest),
                 tocPageNo);
            return nullptr;
        }
        auto* copy = new PageDestination();
        copy->kind = k;
        copy->pageNo = pageNo;
        copy->rect = dest->GetRect();
        copy->zoom = dest->GetZoom();
        copy->value = str::Dup(dest->GetValue());
        copy->name = str::Dup(dest->GetName());
        copy->loc = dest->loc;
        return copy;
    }
    // mupdf, djvu, none → page navigation snapshot
    int pageNo = PageDestGetPageNo(dest);
    if (pageNo <= 0) {
        pageNo = tocPageNo;
    }
    if (pageNo < 1) {
        Str val = dest->GetValue();
        if (val && IsExternalUrl(val)) {
            return new PageDestinationURL(val);
        }
        logf("SnapshotDestForDeferredNav: skip dest kind pageNo=%d (tocPageNo=%d)\n", PageDestGetPageNo(dest),
             tocPageNo);
        return nullptr;
    }
    RectF r = dest->GetRect();
    float zoom = dest->GetZoom();
    if (k == kindDestinationMupdf) {
        // Prefer resolved anchor; outline x/y can be 0 and scroll to the wrong place
        RectF pt = PageDestGetDestPoint(dest);
        if ((r.dx == 0 && r.dy == 0) || (r.dx == kDestUseDefault && r.dy == kDestUseDefault)) {
            if (pt.x != 0 || pt.y != 0 || r.IsEmpty()) {
                r = RectF{pt.x, pt.y, kDestUseDefault, kDestUseDefault};
            }
        }
        zoom = dest->GetZoom();
    }
    IPageDestination* copy = NewSimpleDest(pageNo, r, zoom);
    copy->loc = dest->loc;
    return copy;
}

TocItem* FindTocItemByTitlePage(TocItem* item, Str title, int pageNo) {
    for (; item; item = item->next) {
        if (pageNo > 0 && item->pageNo != pageNo) {
            // keep searching children; same page can nest under different titles
        } else if (title && item->title && str::Eq(title, item->title)) {
            if (pageNo <= 0 || item->pageNo == pageNo) {
                return item;
            }
        } else if (len(title) == 0 && pageNo > 0 && item->pageNo == pageNo) {
            return item;
        }
        TocItem* found = FindTocItemByTitlePage(item->child, title, pageNo);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

// URL / file / embedded targets keep pageNo = -1 by design; only page-nav dests need pageNo >= 1.
static bool DestNeedsValidPageNo(IPageDestination* dest) {
    if (!dest) {
        return false;
    }
    Kind k = dest->GetKind();
    return !IsLaunchLinkKind(k) && k != kindDestinationLaunchEmbedded && k != kindDestinationAttachment;
}

GoToTocLinkData* NewGoToTocLinkData(MainWindow* win, TocItem* tocItem, bool selectInTree) {
    DocController* ctrl = win->ctrl;
    WindowTab* tab = win->CurrentTab();
    if (!ctrl || !tab || tab->ctrl != ctrl) {
        return nullptr;
    }

    int pageNo = tocItem->pageNo;
    IPageDestination* origDest = tocItem->dest;
    if (origDest && pageNo < 1) {
        // chaptered docs: pageNo stays -1 until the target chapter lays out.
        // Resolve now, on the UI thread, so ResolveDest can cache the real
        // page/loc on origDest before it gets snapshotted below -- otherwise
        // SnapshotDestForDeferredNav has nothing but pageNo == -1 to go on
        // and drops the destination.
        Location loc = ctrl->ResolveDest(origDest);
        if (loc.IsValid()) {
            pageNo = ctrl->PageNoFromLocation(loc);
        }
    }
    IPageDestination* dest = SnapshotDestForDeferredNav(origDest, pageNo);

    // drop page-navigation destinations that still have no valid page and no
    // chapter to resolve lazily (see DocController::ResolveDest)
    if (dest && DestNeedsValidPageNo(dest) && PageDestGetPageNo(dest) < 1 && dest->loc.chapter < 1) {
        logf("NewGoToTocLinkData: skip dest with pageNo=%d\n", PageDestGetPageNo(dest));
        delete dest;
        dest = nullptr;
    }

    // nothing to navigate to: no dest and no valid page number
    if (!dest && pageNo < 1) {
        logf("NewGoToTocLinkData: skip toc item pageNo=%d title='%s'\n", pageNo, tocItem->title);
        return nullptr;
    }

    auto* data = new GoToTocLinkData;
    data->ctrl = ctrl;
    data->tab = tab;
    data->pageNo = pageNo;
    data->dest = dest;
    data->selectInTree = selectInTree;
    if (selectInTree && tocItem->title) {
        data->title = str::Dup(tocItem->title);
    }
    return data;
}

bool IsScrollToLink(IPageDestination* link) {
    if (!link) {
        return false;
    }
    const auto* kind = link->GetKind();
    return kind == kindDestinationScrollTo;
}

// the bookmarks that point at pageNo, in tree order
void CollectTocItemsOnPage(TreeModel* tm, int pageNo, Vec<TocItem*>& out) {
    for (TreeItem ti : TreeModelItems(tm)) {
        auto* tocItem = (TocItem*)ti;
        if (tocItem && tocItem->pageNo == pageNo && pageNo >= 1) {
            VecAppend(out, tocItem);
        }
    }
}

bool TocMatchingItemsContains(const Vec<TocItem*>& items, TocItem* item) {
    for (TocItem* t : items) {
        if (t == item) {
            return true;
        }
    }
    return false;
}

bool TocMatchingItemsEq(const Vec<TocItem*>& a, const Vec<TocItem*>& b) {
    if (len(a) != len(b)) {
        return false;
    }
    for (int i = 0; i < len(a); i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool TocItemIsMultiHighlight(MainWindow* win, TocItem* item) {
    if (!gShowAllMatchingTOC || !win || !item) {
        return false;
    }
    return TocMatchingItemsContains(win->tocMatchingItems, item);
}

static bool inRange(WCHAR c, WCHAR low, WCHAR hi) {
    return (low <= c) && (c <= hi);
}

// copied from mupdf/fitz/dev_text.c
// clang-format off
static bool isLeftToRightChar(WCHAR c) {
    return (
        inRange(c, 0x0041, 0x005A) ||
        inRange(c, 0x0061, 0x007A) ||
        inRange(c, 0xFB00, 0xFB06)
    );
}

static bool isRightToLeftChar(WCHAR c) {
    return (
        inRange(c, 0x0590, 0x05FF) ||
        inRange(c, 0x0600, 0x06FF) ||
        inRange(c, 0x0750, 0x077F) ||
        inRange(c, 0xFB50, 0xFDFF) ||
        inRange(c, 0xFE70, 0xFEFE)
    );
}

void GetLeftRightCounts(TocItem* node, int& l2r, int& r2l) {
next:
    if (!node) {
        return;
    }
    // short-circuit because this could overflow the stack due to recursion
    // (happened in doc from https://github.com/sumatrapdfreader/sumatrapdf/issues/1795)
    if (l2r + r2l > 1024) {
        return;
    }
    if (node->title) {
        TempWStr ws = ToWStrTemp(node->title);
        for (int i = 0; i < ws.len; i++) {
            WCHAR c = ws.s[i];
            if (isLeftToRightChar(c)) {
                l2r++;
            } else if (isRightToLeftChar(c)) {
                r2l++;
            }
        }
    }
    GetLeftRightCounts(node->child, l2r, r2l);
    // could be: GetLeftRightCounts(node->next, l2r, r2l);
    // but faster if not recursive
    node = node->next;
    goto next;
}

void SetInitialExpandState(TocItem* item, Vec<int>& tocState) {
    while (item) {
        item->isOpenToggled = VecContains(tocState, item->id);
        SetInitialExpandState(item->child, tocState);
        item = item->next;
    }
}

void AddFavoriteFromToc(MainWindow* win, TocItem* dti) {
    int pageNo = 0;
    if (!dti) {
        return;
    }
    if (dti->dest) {
        pageNo = PageDestGetPageNo(dti->dest);
    }
    Str name = dti->title;
    TempStr pageLabel = win->ctrl->GetPageLabeTemp(pageNo);
    AddFavoriteWithLabelAndName(win, pageNo, pageLabel, name);
}

// auto-expand root level ToC nodes if there are at most two
void AutoExpandTopLevelItems(TocItem* root) {
    if (!root) {
        return;
    }
    if (root->next && root->next->next) {
        return;
    }

    if (!root->IsExpanded()) {
        root->isOpenToggled = !root->isOpenToggled;
    }
    if (!root->next) {
        return;
    }
    if (!root->next->IsExpanded()) {
        root->next->isOpenToggled = !root->next->isOpenToggled;
    }
}

TocItem* FindTocItemByTitleAndPage(TocItem* item, Str title, int pageNo) {
    while (item) {
        if (item->pageNo == pageNo && str::Eq(item->title, title)) {
            return item;
        }
        if (TocItem* found = FindTocItemByTitleAndPage(item->child, title, pageNo)) {
            return found;
        }
        item = item->next;
    }
    return nullptr;
}

// Append a TocItem linked list onto resultFirst/resultLast (updates last).
static void AppendTocSiblingList(TocItem*& resultFirst, TocItem*& resultLast, TocItem* list) {
    if (!list) {
        return;
    }
    if (!resultFirst) {
        resultFirst = list;
    } else {
        resultLast->next = list;
    }
    resultLast = list;
    while (resultLast->next) {
        resultLast = resultLast->next;
    }
}

// Recursively build a filtered copy of the TocItem tree.
// Multi-word filter (command palette style): every word must appear in the
// item's own title to keep that node. Non-matching ancestors are omitted and
// matching descendants are promoted so only fully-matching rows are shown.
// Returns nullptr if nothing matches.
TocItem* FilterTocItemRec(TocItem* item, const StrVec& words) {
    if (!item) {
        return nullptr;
    }
    TocItem* resultFirst = nullptr;
    TocItem* resultLast = nullptr;
    for (TocItem* si = item; si; si = si->next) {
        TocItem* filteredChildren = FilterTocItemRec(si->child, words);
        bool titleMatches = si->title && FilterMatches(si->title, words);
        if (titleMatches) {
            // keep this node; only fully-matching children stay nested under it
            auto* copy = AllocTocItem(nullptr, si->title, si->pageNo);
            copy->id = si->id;
            copy->fontFlags = si->fontFlags;
            copy->color = si->color;
            copy->dest = si->dest;
            copy->destNotOwned = true;
            copy->isOpenDefault = true;
            copy->isOpenToggled = false;
            copy->child = filteredChildren;
            for (TocItem* c = copy->child; c; c = c->next) {
                c->parent = copy;
            }
            AppendTocSiblingList(resultFirst, resultLast, copy);
        } else if (filteredChildren) {
            // title does not match every word: drop this node, promote children
            for (TocItem* c = filteredChildren; c; c = c->next) {
                c->parent = nullptr;
            }
            AppendTocSiblingList(resultFirst, resultLast, filteredChildren);
        }
    }
    return resultFirst;
}

// navigate to a TocItem regardless of whether it points to a page in this
// document or to an external destination (used by the command palette, where
// the user explicitly picked the item so we always honor it)
// navigate to a TocItem (used by the command palette's TOC mode)
void GoToTocItem(MainWindow* win, TocItem* tocItem) {
    if (!win || !tocItem) {
        return;
    }
    auto* data = NewGoToTocLinkData(win, tocItem, true);
    if (!data) {
        return;
    }
    auto fn = MkFunc0<GoToTocLinkData>(GoToTocLink, data);
    uitask::Post(fn, "TaskGoToTocFromPalette");
}

// find the closest item in tree view to a given page number
TocItem* TreeItemForPageNo(TreeModel* tm, int pageNo) {
    if (!tm) {
        return nullptr;
    }
    TocItem* bestMatch = nullptr;
    int bestMatchPageNo = 0;
    int nItems = 0;
    for (TreeItem ti : TreeModelItems(tm)) {
        auto* tocItem = (TocItem*)ti;
        if (!tocItem) {
            continue;
        }
        if (!bestMatch) {
            // if nothing else matches, match the root node
            bestMatch = tocItem;
        }
        ++nItems;
        int page = tocItem->pageNo;
        if ((page <= pageNo) && (page >= bestMatchPageNo) && (page >= 1)) {
            bestMatch = tocItem;
            bestMatchPageNo = page;
            if (pageNo == bestMatchPageNo) {
                // we can stop earlier if we found the exact match
                break;
            }
        }
    }
    // if there's only one item, we want to unselect it so that it can
    // be selected by the user
    if (nItems < 2) {
        return nullptr;
    }
    return bestMatch;
}

// closest item (in tree order) whose loc.chapter matches, preferring an exact
// pageNo match; chaptered docs need chapter-first matching because unresolved
// items all share pageNo == -1, so TreeItemForPageNo can't tell them apart
TocItem* TreeItemForChapter(TreeModel* tm, int chapter, int pageNo) {
    if (!tm || chapter < 1) {
        return nullptr;
    }
    TocItem* match = nullptr;
    int matchPageNo = 0;
    for (TreeItem ti : TreeModelItems(tm)) {
        auto* tocItem = (TocItem*)ti;
        if (!tocItem || tocItem->loc.chapter != chapter) {
            continue;
        }
        if (!match) {
            match = tocItem;
            matchPageNo = tocItem->pageNo;
        }
        int page = tocItem->pageNo;
        if (page >= 1 && page <= pageNo && page >= matchPageNo) {
            match = tocItem;
            matchPageNo = page;
            if (page == pageNo) {
                break;
            }
        }
    }
    return match;
}

void OpenAttachment(WindowTab* tab, Str fileName, int attachmentNo) {
    if (!tab || !tab->AsFixed()) {
        return;
    }
    EngineBase* engine = tab->AsFixed()->GetEngine();
    Str data = EngineMupdfLoadAttachment(engine, attachmentNo);
    if (len(data) == 0) {
        return;
    }
    OpenDocumentFromMemory(tab->win, data, fileName);
    str::Free(data);
}

void GoToTocTreeItem(MainWindow* win, TocItem* tocItem, bool allowExternal) {
    if (!tocItem) {
        return;
    }
    bool validPage = (tocItem->pageNo > 0);
    bool isScroll = IsScrollToLink(tocItem->dest);
    bool hasChapterDest = tocItem->dest && tocItem->dest->loc.chapter >= 1;
    if (validPage || allowExternal || isScroll || hasChapterDest) {
        // delay changing the page until the tree messages have been handled
        auto* data = NewGoToTocLinkData(win, tocItem, false);
        if (!data) {
            return;
        }
        auto fn = MkFunc0<GoToTocLinkData>(GoToTocLink, data);
        uitask::Post(fn, "TaskGoToTocTreeItem");
    }
}

//--- bookmarks as plain text (see the format description in the header)

// A name may hold any character, so quote the two that would end or confuse the
// field, plus the control characters that would break the line.
static void BookmarkTextAppendEscaped(str::Builder& b, Str s) {
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        switch (c) {
            case '\\':
                b.Append(StrL("\\\\"));
                break;
            case '"':
                b.Append(StrL("\\\""));
                break;
            case '\n':
                b.Append(StrL("\\n"));
                break;
            case '\r':
                b.Append(StrL("\\r"));
                break;
            case '\t':
                b.Append(StrL("\\t"));
                break;
            default:
                b.AppendChar(c);
                break;
        }
    }
}

static void BookmarkTextAppendLevel(str::Builder& b, TocItem* item, int depth) {
    for (; item; item = item->next) {
        if (TocItemIsAttachment(item)) {
            continue;
        }
        for (int i = 0; i < depth; i++) {
            b.AppendChar('\t');
        }
        b.AppendChar('"');
        BookmarkTextAppendEscaped(b, item->title);
        // pageNo is 1-based; 0 means the entry has no destination
        b.Append(fmt("\" %d\n", item->pageNo > 0 ? item->pageNo : 0));
        if (item->child) {
            BookmarkTextAppendLevel(b, item->child, depth + 1);
        }
    }
}

Str BookmarkTextFromToc(TocItem* first) {
    str::Builder b;
    b.Reserve(1024);
    BookmarkTextAppendLevel(b, first, 0);
    return b.TakeStr();
}

static bool IsPageSep(char c) {
    return c == ' ' || c == '\t';
}

// Read one line, building a TocItem for it. Returns "" on success, or why the
// line couldn't be read.
static TempStr BookmarkTextParseLine(Str line, int lineNo, TocItem* parent, int depth, TocItem** itemOut) {
    int i = 0;
    while (i < len(line) && line.s[i] == '\t') {
        i++;
    }
    if (i != depth) {
        return fmt("line %d: expected %d leading tab(s)", lineNo, depth);
    }
    if (i >= len(line) || line.s[i] != '"') {
        return fmt("line %d: expected a quoted name", lineNo);
    }
    i++;

    str::Builder name;
    bool closed = false;
    while (i < len(line)) {
        char c = line.s[i];
        if (c == '"') {
            closed = true;
            i++;
            break;
        }
        if (c != '\\') {
            name.AppendChar(c);
            i++;
            continue;
        }
        if (i + 1 >= len(line)) {
            break;
        }
        char e = line.s[i + 1];
        if (e == '"') {
            name.AppendChar('"');
        } else if (e == '\\') {
            name.AppendChar('\\');
        } else if (e == 'n') {
            name.AppendChar('\n');
        } else if (e == 'r') {
            name.AppendChar('\r');
        } else if (e == 't') {
            name.AppendChar('\t');
        } else {
            char esc[2] = {e, 0};
            return fmt("line %d: unknown escape '\\%s'", lineNo, Str(esc, 1));
        }
        i += 2;
    }
    if (!closed) {
        return fmt("line %d: unterminated name (missing closing quote)", lineNo);
    }

    if (i >= len(line) || !IsPageSep(line.s[i])) {
        return fmt("line %d: expected whitespace and a page number after the name", lineNo);
    }
    while (i < len(line) && IsPageSep(line.s[i])) {
        i++;
    }
    // a page number we'd never reach; guards against garbage and overflow
    constexpr int kMaxBookmarkPage = 1000000;
    int pageNo = 0;
    bool anyDigit = false;
    while (i < len(line) && line.s[i] >= '0' && line.s[i] <= '9') {
        anyDigit = true;
        pageNo = pageNo * 10 + (line.s[i] - '0');
        if (pageNo > kMaxBookmarkPage) {
            return fmt("line %d: page number is too large", lineNo);
        }
        i++;
    }
    if (!anyDigit) {
        return fmt("line %d: expected a page number", lineNo);
    }
    while (i < len(line) && IsPageSep(line.s[i])) {
        i++;
    }
    if (i < len(line)) {
        return fmt("line %d: unexpected text after the page number", lineNo);
    }

    TocItem* item = AllocTocItem(nullptr, ToStr(name), pageNo);
    item->parent = parent;
    *itemOut = item;
    return {};
}

TempStr BookmarkTextParse(Str text, TocItem** rootInOut) {
    TocItem* root = nullptr;
    // last entry per depth, so siblings can be appended in order
    Vec<TocItem*> lastAt;
    int lineNo = 0;
    int pos = 0;
    int n = len(text);

    while (pos < n) {
        int lineEnd = pos;
        while (lineEnd < n && text.s[lineEnd] != '\n') {
            lineEnd++;
        }
        Str line(text.s + pos, lineEnd - pos);
        pos = lineEnd + 1;
        lineNo++;
        if (len(line) > 0 && line.s[len(line) - 1] == '\r') {
            line.len--;
        }

        int i = 0;
        while (i < len(line) && line.s[i] == '\t') {
            i++;
        }
        // blank line or comment
        if (i == len(line) || line.s[i] == '#') {
            continue;
        }

        int depth = i;
        // mupdf stops nesting at 64 too (BuildTocTree), and everything below
        // walks the tree recursively
        constexpr int kMaxBookmarkDepth = 63;
        if (depth > kMaxBookmarkDepth) {
            return fmt("line %d: nested too deeply", lineNo);
        }
        if (depth > len(lastAt)) {
            return fmt("line %d: indent jumps more than one level", lineNo);
        }
        TocItem* parent = depth == 0 ? nullptr : lastAt[depth - 1];

        TocItem* item = nullptr;
        TempStr err = BookmarkTextParseLine(line, lineNo, parent, depth, &item);
        if (err) {
            FreeTocItemRec(nullptr, root);
            return err;
        }

        if (depth == 0) {
            if (root) {
                root->AddSiblingAtEnd(item);
            } else {
                root = item;
            }
        } else {
            TocItem* p = lastAt[depth - 1];
            if (p->child) {
                p->child->AddSiblingAtEnd(item);
            } else {
                p->child = item;
            }
        }
        // a new entry becomes the anchor for deeper ones and for its siblings
        if (len(lastAt) == depth) {
            VecAppend(lastAt, item);
        } else {
            lastAt[depth] = item;
            lastAt.len = depth + 1;
        }
    }

    if (!root) {
        return fmt("no bookmarks in the file");
    }
    *rootInOut = root;
    return {};
}
