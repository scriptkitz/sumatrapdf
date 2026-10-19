/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/CmdLineArgs.h"
#include "base/UITask.h"
#include "EmbeddedResources.h"
#if OS_POSIX && !OS_WASM
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
#endif
#include "PDFScript.h"

static StrVec gScriptNames;
static StrVec gScriptSources;

// Scripts that ship inside the binary. The bundle in scripts.js is generated
// from the sumatra-website checkout by cmd/gen-pdf-scripts.ts and is simply
// absent from a build made without that checkout, so a script this build must
// always offer lives here rather than in a file.
static const Str kBuiltinPdfScripts[] = {
    StrL(R"JS(// Info: Remove watermark
// Arg: <INPUT> : input PDF
// Arg: <OUTPUT> : where to write the result
// Arg: 0 : 1 = also drop curve operators from the page content streams
//
// Standalone example, run it with:
//
//   SumatraPDF.exe run remove-watermark.js in.pdf out.pdf
//
// Two ways of losing a watermark, in order of preference:
//
// 1. Identified ones. Foxit's editors stamp their watermark form XObjects with
//    /PieceInfo /ADBE_CompoundType /Private /FreeWatermark. We rename
//    /Subtype from /Form to a name readers ignore (pdf-interpret.c only acts on
//    Form and Image), so the XObject stays in the file and the artwork is still
//    there if this guess was wrong.
//
// 2. Everything else, by dropping curve operators from the page content
//    streams. This is a blunt instrument: it removes every bezier curve on the
//    page, not only the watermark, and it is off unless scriptArgs[2] asks for
//    it. It is what the PyMuPDF original did.
//
//    Also note that it works line by line, so it needs a content stream written
//    one operator per line - which is how most generators write them, and the
//    only shape the original's `endswith(" c")` test could ever match. A
//    minified stream is left alone.

var doc = Document.openDocument(scriptArgs[0]);
if (!doc.isPDF()) {
  print("not a PDF: " + scriptArgs[0]);
  quit();
}

var kFreeWatermark = "FreeWatermark";

function isName(obj, name) {
  return obj && obj.isName() && obj.asName() == name;
}

// The private data is a name in some files, a string in others.
function isFreeWatermark(obj) {
  if (!obj) return false;
  if (obj.isName()) return obj.asName() == kFreeWatermark;
  if (obj.isString()) return obj.asString() == kFreeWatermark;
  return false;
}

function hideFreeWatermarks() {
  var hidden = 0;
  var n = doc.countPages();
  for (var i = 0; i < n; i++) {
    var resources = doc.loadPage(i).getObject().get("Resources");
    if (!resources) continue;
    var xobjs = resources.get("XObject");
    if (!xobjs) continue;
    xobjs.forEach(function (xobj, name) {
      if (!xobj || !xobj.isDictionary()) return;
      if (!isName(xobj.get("Subtype"), "Form")) return;
      var pieceInfo = xobj.get("PieceInfo");
      if (!pieceInfo) return;
      var compound = pieceInfo.get("ADBE_CompoundType");
      if (!compound) return;
      if (!isFreeWatermark(compound.get("Private"))) return;
      xobj.put("Subtype", "Test");
      hidden++;
    });
  }
  return hidden;
}

// content stream filtering

// Operators that add a curve segment, plus the ones that start a subpath and
// paint it. Dropping "m" and "h" as well is what the original script did; it
// turns filled curve shapes into nothing but leaves straight-line art alone.
var kCurveOps = { c: 1, v: 1, y: 1 };
var kCurveAndPathOps = { c: 1, v: 1, y: 1, m: 1, h: 1, "f*": 1 };

function isDropOp(line, ops) {
  // the trailing operator is the last run of non-blank characters
  var s = line.replace(/[ \t\r]+$/, "");
  var i = s.length;
  while (i > 0) {
    var c = s.charAt(i - 1);
    if (c == " " || c == "\t" || c == "\r") break;
    i--;
  }
  var op = s.substring(i);
  return op.length > 0 && ops[op] ? 1 : 0;
}

// Runs of whitespace and comments are left as they are; only whole lines whose
// operator is dropped disappear, which keeps the operand-free operators
// around them (q, Q, BT, ET, ...) balanced.
function stripOps(text, ops) {
  var lines = text.split("\n");
  var out = [];
  var dropped = 0;
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i];
    var trimmed = line.replace(/^\s+/, "");
    if (trimmed.length == 0 || trimmed.charAt(0) == "%") {
      out.push(line);
      continue;
    }
    if (isDropOp(line, ops)) {
      dropped++;
      continue;
    }
    out.push(line);
  }
  return { text: out.join("\n"), dropped: dropped };
}

function filterPageContents(ops) {
  var dropped = 0;
  var n = doc.countPages();
  for (var i = 0; i < n; i++) {
    var pageObj = doc.loadPage(i).getObject();
    var contents = pageObj.get("Contents");
    if (!contents) continue;

    // /Contents may be one stream or an array of them, and operators may span
    // the split, so decode every part and filter the joined text. forEach over
    // a plain stream walks its dictionary keys, so branch on isArray first.
    var parts = [];
    var collect = function (stream) {
      if (!stream || !stream.isStream()) return;
      var part = stream.readStream();
      if (part) parts.push(part.asString());
    };
    if (contents.isArray()) contents.forEach(collect);
    else collect(contents);
    if (!parts.length) continue;

    var joined = parts.join("\n");
    var res = stripOps(joined, ops);
    if (!res.dropped) continue;
    dropped += res.dropped;

    // one rebuilt stream replaces the whole array
    var buf = new Buffer();
    buf.write(res.text);
    pageObj.put("Contents", doc.addStream(buf));
  }
  return dropped;
}

// main

var hidden = hideFreeWatermarks();
print("watermark XObjects hidden: " + hidden);

var mode = scriptArgs.length > 2 ? parseInt(scriptArgs[2], 10) : 0;
if (mode) {
  var ops = mode > 1 ? kCurveAndPathOps : kCurveOps;
  var dropped = filterPageContents(ops);
  print("content operators dropped: " + dropped);
} else if (!hidden) {
  print("nothing identified; run again with 1 as the third argument to strip curve operators");
}

doc.save(scriptArgs[1], "compress");
print("written: " + scriptArgs[1]);
)JS"),
};

static void LoadPDFScripts() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    StrVec sources;
    int size = 0;
    u8* data = GetEmbeddedFileData(StrL("scripts.js"), &size);
    if (data) {
        Split(&sources, Str((char*)data, size), StrL("// ---"));
        free(data);
    }
    for (Str builtin : kBuiltinPdfScripts) {
        sources.Append(builtin);
    }

    const Str prefix = StrL("// Info:");
    for (Str source : sources) {
        Str line;
        Str rest = source;
        while (str::NextLine(rest, line, rest) && str::IsEmptyOrWhiteSpace(line)) {
            source = rest;
        }
        gScriptSources.Append(source);

        Str name;
        rest = source;
        while (str::NextLine(rest, line, rest)) {
            line = str::DupTemp(line);
            str::TrimWSInPlace(line, str::TrimOpt::Both);
            if (!str::StartsWith(line, prefix)) continue;
            name = Str(line.s + len(prefix), len(line) - len(prefix));
            str::TrimWSInPlace(name, str::TrimOpt::Both);
            if (str::EndsWith(name, StrL("."))) name.len--;
            break;
        }
        if (len(name) == 0) name = fmt("Script %d", len(gScriptNames) + 1);
        gScriptNames.Append(name);
    }
}

int PDFScriptCount() {
    LoadPDFScripts();
    return len(gScriptSources);
}

Str PDFScriptName(int index) {
    return index >= 0 && index < PDFScriptCount() ? gScriptNames[index] : Str();
}

Str PDFScriptSource(int index) {
    return index >= 0 && index < PDFScriptCount() ? gScriptSources[index] : Str();
}

PDFScriptResult::~PDFScriptResult() {
    str::Free(output);
}

TempStr PDFScriptOutputPath(Str input) {
    if (len(input) == 0) return {};
    Str base = path::GetPathNoExtTemp(input);
    Str ext = path::GetExtTemp(input);
    for (int i = 1;; i++) {
        TempStr output = fmt("%s.%d%s", base, i, ext);
        if (!file::Exists(output)) return output;
    }
}

// A trailing "(...)" is a note in the script, not a dialog label.
static TempStr StripTrailingComment(Str s) {
    TempStr out = str::DupTemp(s);
    if (!str::EndsWith(out, StrL(")"))) {
        return out;
    }
    int i = str::LastIndexOfChar(out, '(');
    if (i < 0) {
        return out;
    }
    out.len = i;
    str::TrimWSInPlace(out, str::TrimOpt::Right);
    return out;
}

static TempStr ExpandScriptArg(Str value, Str input, Str output) {
    // "<DIR>/rest" is a path: the slash is the separator, so Windows gets "\"
    TempStr dir = path::GetDirTemp(input);
    TempStr dirSep = path::JoinTemp(dir, StrL(""));
    TempStr expanded = str::ReplaceTemp(value, StrL("<DIR>/"), dirSep);
    expanded = str::ReplaceTemp(expanded, StrL("<DIR>\\"), dirSep);
    expanded = str::ReplaceTemp(expanded, StrL("<DIR>"), dir);
    expanded = str::ReplaceTemp(expanded, StrL("<INPUT>"), input);
    expanded = str::ReplaceTemp(expanded, StrL("<OUTPUT>"), output);
    return str::ReplaceTemp(expanded, StrL("<BASE>"), path::GetBaseNameTemp(path::GetPathNoExtTemp(input)));
}

void GetPDFScriptArgs(Str script, Str input, StrVec& values, StrVec& hints) {
    values.Reset();
    hints.Reset();
    Str output = PDFScriptOutputPath(input);
    const Str prefix = StrL("// Arg:");
    Str line;
    Str rest = script;
    while (str::NextLine(rest, line, rest)) {
        line = str::DupTemp(line);
        str::TrimWSInPlace(line, str::TrimOpt::Both);
        if (!str::StartsWith(line, prefix)) continue;
        Str spec(line.s + len(prefix), len(line) - len(prefix));
        Str value;
        Str hint;
        if (!str::Cut(spec, StrL(" : "), &value, &hint)) hint = spec;
        str::TrimWSInPlace(value, str::TrimOpt::Both);
        str::TrimWSInPlace(hint, str::TrimOpt::Both);
        values.Append(ExpandScriptArg(value, input, output));
        hints.Append(StripTrailingComment(hint));
    }
}

// Keep path quoting in JavaScript: "<INPUT>" also works for Windows backslashes and quotes.
// Empty must be a real string: ReplaceTemp drops the script when the replacement is null.
static TempStr ScriptPathTemp(Str path) {
    if (len(path) == 0) {
        return str::DupTemp(StrL(""));
    }

    str::Builder out;
    for (int i = 0; i < len(path); i++) {
        char c = path.s[i];
        switch (c) {
            case '\\':
                out.Append(StrL("\\\\"));
                break;
            case '"':
                out.Append(StrL("\\\""));
                break;
            case '\'':
                out.Append(StrL("\\'"));
                break;
            case '\n':
                out.Append(StrL("\\n"));
                break;
            case '\r':
                out.Append(StrL("\\r"));
                break;
            default:
                out.AppendChar(c);
                break;
        }
    }
    return str::DupTemp(ToStr(out));
}

TempStr ExpandPDFScript(Str script, const StrVec& args) {
    Str input = len(args) > 0 ? args[0] : Str();
    Str output = len(args) > 1 ? args[1] : Str();
    TempStr expanded = str::ReplaceTemp(script, StrL("<INPUT>"), ScriptPathTemp(input));
    return str::ReplaceTemp(expanded, StrL("<OUTPUT>"), ScriptPathTemp(output));
}

struct PDFScriptTask {
    Str script;
    Str exe;
    StrVec args;
    Func1<PDFScriptResult*> onDone;
    PDFScriptResult* result = new PDFScriptResult();
    ~PDFScriptTask() {
        str::Free(script);
        str::Free(exe);
        delete result;
    }
};

// A child process isolates MuJS and its process-wide stdout/stderr from the viewer.
static int RunScriptChild(Str exe, Str scriptPath, Str outputPath, const StrVec& args) {
#if OS_WIN
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE output = CreateFileW(CWStrTemp(outputPath), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        return -1;
    }
    str::Builder cmd;
    cmd.Append(fmt("%s run %s", QuoteCmdLineArgTemp(exe), QuoteCmdLineArgTemp(scriptPath)));
    for (Str arg : args) {
        cmd.Append(fmt(" %s", QuoteCmdLineArgTemp(arg.s ? arg : StrL(""))));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = input;
    si.hStdOutput = si.hStdError = output;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(CWStrTemp(exe), CWStrTemp(ToStr(cmd)), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             nullptr, &si, &pi);
    CloseHandle(input);
    CloseHandle(output);
    if (!ok) return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
#elif OS_POSIX && !OS_WASM
    StrVec argvStrings;
    argvStrings.Append(exe);
    argvStrings.Append(StrL("run"));
    argvStrings.Append(scriptPath);
    for (Str arg : args) argvStrings.Append(arg);
    Vec<char*> argv;
    for (Str arg : argvStrings) VecAppend(argv, CStrTemp(arg.s ? arg : StrL("")));
    VecAppend(argv, (char*)nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, CStrTemp(outputPath), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = 0;
    int err = posix_spawn(&pid, CStrTemp(exe), &actions, nullptr, argv.els, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (err) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#else
    return -1;
#endif
}

static void FinishPDFScript(PDFScriptTask* task) {
    PDFScriptResult* result = task->result;
    task->result = nullptr;
    task->onDone.Call(result);
    delete task;
}

static void RunPDFScriptTask(PDFScriptTask* task) {
    TempStr scriptPath = GetTempFilePathTemp(StrL("pdf-script"));
    TempStr outputPath = GetTempFilePathTemp(StrL("pdf-output"));
    if (scriptPath && outputPath && file::WriteFile(scriptPath, task->script)) {
        task->result->exitCode = RunScriptChild(task->exe, scriptPath, outputPath, task->args);
        task->result->output = file::ReadFile(outputPath);
    }
    if (scriptPath) file::Delete(scriptPath);
    if (outputPath) file::Delete(outputPath);
    if (task->result->exitCode != 0) {
        str::Builder out;
        out.Append(task->result->output);
        out.Append(fmt("\nScript exited with code %d.\n", task->result->exitCode));
        str::ReplaceWithCopy(&task->result->output, ToStr(out));
    }
    uitask::Post(MkFunc0(FinishPDFScript, task));
}

void StartPDFScript(Str script, const StrVec& args, const Func1<PDFScriptResult*>& onDone) {
    auto* task = new PDFScriptTask();
    task->script = str::Dup(ExpandPDFScript(script, args));
    task->exe = str::Dup(GetSelfExePathTemp());
    for (Str arg : args) task->args.Append(arg);
    task->onDone = onDone;
    ThreadHandle thread = StartThread(MkFunc0(RunPDFScriptTask, task), StrL("PDFScript"));
    if (!thread) {
        task->result->output = str::Dup(StrL("Could not start script runner."));
        uitask::Post(MkFunc0(FinishPDFScript, task));
        return;
    }
    SafeCloseThreadHandle(&thread);
}
