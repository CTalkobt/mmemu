#include "source_pane.h"
#include "libdebug/main/debug_context.h"
#include "libtoolchain/main/source_map.h"
#include "libdebug/main/source_stepper.h"
#include <wx/settings.h>
#include <algorithm>
#include <filesystem>

namespace {

// Margins and markers
constexpr int kMarginLines   = 0;
constexpr int kMarginSymbols = 1;
constexpr int kMarkBreakpoint = 0;
constexpr int kMarkExecArrow  = 1;
constexpr int kMarkExecLine   = 2;

// 6502 / 65CE02 / 45GS02 mnemonics (lowercase: the asm lexer lowercases words)
const char* kAsmMnemonics =
    "adc and asl bcc bcs beq bit bmi bne bpl brk bvc bvs clc cld cli clv cmp cpx cpy "
    "dec dex dey eor inc inx iny jmp jsr lda ldx ldy lsr nop ora pha php pla plp rol ror "
    "rti rts sbc sec sed sei sta stx sty tax tay tsx txa txs tya "
    "bra phx phy plx ply stz trb tsb "
    "asr asw aug bbr0 bbr1 bbr2 bbr3 bbr4 bbr5 bbr6 bbr7 bbs0 bbs1 bbs2 bbs3 bbs4 bbs5 bbs6 bbs7 "
    "bsr cle cpz dew dez dez inw inz ldz map neg phw phz plz rmb0 rmb1 rmb2 rmb3 rmb4 rmb5 rmb6 rmb7 "
    "row rtn see smb0 smb1 smb2 smb3 smb4 smb5 smb6 smb7 tab taz tba tsy tys tza eom "
    "lbcc lbcs lbeq lbmi lbne lbpl lbra lbvc lbvs "
    "adcq andq aslq asrq bitq cmpq deq eorq inq ldq lsrq oraq rolq rorq sbcq stq";
const char* kAsmRegisters = "a x y z b s sp q";
const char* kAsmDirectives =
    ".byte .word .dword .text .ascii .asciiz .petscii .screen .fill .res .align .org .include "
    ".incbin .if .else .endif .ifdef .ifndef .macro .endmacro .endm .proc .endproc .segment "
    ".const .var .label .define .import .export .global .zp .data .code .bss .cpu .encoding "
    "* = .equ .set .end";

const char* kBasicKeywords =
    "end for next data input# input dim read let goto run if restore gosub return rem stop on wait "
    "load save verify def poke print# print cont list clr cmd sys open close get new tab( to fn spc( "
    "then not step and or sgn int abs usr fre pos sqr rnd log exp cos sin tan atn peek len str$ val "
    "asc chr$ left$ right$ mid$ go "
    // BASIC 65 additions (common ones)
    "else resume trap tron troff delete renumber key monitor using until while do loop exit "
    "begin bend scratch directory dopen dclose append dsave dload catalog rename backup collect "
    "copy concat bload bsave fast slow graphic palette screen line circle box paint sprite movspr "
    "sound play tempo vol envelope filter color background border foreground fread fwrite";

const char* kCKeywords =
    "auto break case char const continue default do double else enum extern float for goto if "
    "inline int long register restrict return short signed sizeof static struct switch typedef "
    "union unsigned void volatile while _Bool";

enum class Lang { Asm, Basic, C, Text };

Lang languageFor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".s" || ext == ".asm" || ext == ".s45" || ext == ".a65" || ext == ".inc" ||
        ext == ".a" || ext == ".65s")
        return Lang::Asm;
    if (ext == ".bas" || ext == ".b65") return Lang::Basic;
    if (ext == ".c" || ext == ".h")     return Lang::C;
    return Lang::Text;
}

bool isDark(const wxColour& c) {
    return (c.Red() * 299 + c.Green() * 587 + c.Blue() * 114) / 1000 < 128;
}

}  // namespace

SourcePane::SourcePane(wxWindow* parent) : wxPanel(parent, wxID_ANY) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_book = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_SCROLL_BUTTONS |
                               wxAUI_NB_CLOSE_ON_ACTIVE_TAB | wxAUI_NB_WINDOWLIST_BUTTON);
    sizer->Add(m_book, 1, wxEXPAND);
    SetSizer(sizer);

    m_book->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& e) {
        Doc* doc = docForCtrl(m_book->GetPage(e.GetSelection()));
        if (!doc) return;
        if (doc->stc->GetModify()) {
            int answer = wxMessageBox("Save changes to " + wxString::FromUTF8(std::filesystem::path(doc->path).filename().string()) + "?",
                                      "Close Source", wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
            if (answer == wxCANCEL || (answer == wxYES && !saveDoc(*doc))) { e.Veto(); return; }
        }
        if (doc->path == m_execFile) m_execFile.clear();
        std::string path = doc->path;
        m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(),
                                    [&](const Doc& d) { return d.path == path; }), m_docs.end());
    });
}

void SourcePane::SetDebugContext(DebugContext* dbg) {
    m_dbg = dbg;
    m_execFile.clear();
    for (auto& d : m_docs) {
        d.stc->MarkerDeleteAll(kMarkExecArrow);
        d.stc->MarkerDeleteAll(kMarkExecLine);
    }
    RefreshBreakpointMarkers(true);
}

SourcePane::Doc* SourcePane::findDoc(const std::string& path) {
    for (auto& d : m_docs) if (d.path == path) return &d;
    return nullptr;
}

SourcePane::Doc* SourcePane::docForCtrl(wxObject* ctrl) {
    for (auto& d : m_docs) if (d.stc == ctrl) return &d;
    return nullptr;
}

std::string SourcePane::CurrentFile() const {
    int sel = m_book->GetSelection();
    if (sel == wxNOT_FOUND) return "";
    wxWindow* page = m_book->GetPage(sel);
    for (const auto& d : m_docs) if (d.stc == page) return d.path;
    return "";
}

bool SourcePane::OpenFile(const std::string& rawPath) {
    const std::string path = SourceMap::normalizePath(rawPath);
    if (Doc* doc = findDoc(path)) {
        m_book->SetSelection(m_book->GetPageIndex(doc->stc));
        return true;
    }
    auto* stc = new wxStyledTextCtrl(m_book, wxID_ANY);
    if (!stc->LoadFile(wxString::FromUTF8(path))) {
        stc->Destroy();
        if (m_status) m_status("Could not open " + wxString::FromUTF8(path));
        return false;
    }
    configureEditor(stc, path);
    stc->EmptyUndoBuffer();
    stc->SetSavePoint();
    m_docs.push_back({path, stc});
    Doc& doc = m_docs.back();
    m_book->AddPage(stc, wxString::FromUTF8(std::filesystem::path(path).filename().string()), true);
    m_book->SetPageToolTip(m_book->GetPageIndex(stc), wxString::FromUTF8(path));
    updateTabTitle(doc);

    stc->Bind(wxEVT_STC_SAVEPOINTLEFT,    [this, stc](wxStyledTextEvent&) { if (Doc* d = docForCtrl(stc)) updateTabTitle(*d); });
    stc->Bind(wxEVT_STC_SAVEPOINTREACHED, [this, stc](wxStyledTextEvent&) { if (Doc* d = docForCtrl(stc)) updateTabTitle(*d); });
    stc->Bind(wxEVT_STC_MARGINCLICK, &SourcePane::onMarginClick, this);
    RefreshBreakpointMarkers(true);
    return true;
}

void SourcePane::configureEditor(wxStyledTextCtrl* stc, const std::string& path) {
    const wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    const bool dark = isDark(bg);
    const wxColour cComment = dark ? wxColour(0x7F, 0x9F, 0x7F) : wxColour(0x00, 0x80, 0x00);
    const wxColour cKeyword = dark ? wxColour(0x6C, 0xB6, 0xFF) : wxColour(0x00, 0x00, 0xC0);
    const wxColour cNumber  = dark ? wxColour(0xE5, 0xC0, 0x7B) : wxColour(0x80, 0x50, 0x00);
    const wxColour cString  = dark ? wxColour(0xCE, 0x91, 0x78) : wxColour(0xA0, 0x20, 0x20);
    const wxColour cDirect  = dark ? wxColour(0xC5, 0x86, 0xC0) : wxColour(0x80, 0x00, 0x80);

    wxFont mono(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
    stc->StyleSetFont(wxSTC_STYLE_DEFAULT, mono);
    stc->StyleSetBackground(wxSTC_STYLE_DEFAULT, bg);
    stc->StyleSetForeground(wxSTC_STYLE_DEFAULT, fg);
    stc->StyleClearAll();
    stc->StyleSetBackground(wxSTC_STYLE_LINENUMBER, wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
    stc->StyleSetForeground(wxSTC_STYLE_LINENUMBER, wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    stc->SetCaretForeground(fg);
    stc->SetSelBackground(true, wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT));
    stc->SetSelForeground(true, wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT));
    stc->SetTabWidth(8);
    stc->SetUseTabs(false);

    switch (languageFor(path)) {
        case Lang::Asm:
            stc->SetLexer(wxSTC_LEX_ASM);
            stc->SetKeyWords(0, kAsmMnemonics);
            stc->SetKeyWords(2, kAsmRegisters);
            stc->SetKeyWords(3, kAsmDirectives);
            stc->StyleSetForeground(wxSTC_ASM_COMMENT, cComment);
            stc->StyleSetForeground(wxSTC_ASM_COMMENTBLOCK, cComment);
            stc->StyleSetForeground(wxSTC_ASM_NUMBER, cNumber);
            stc->StyleSetForeground(wxSTC_ASM_STRING, cString);
            stc->StyleSetForeground(wxSTC_ASM_CHARACTER, cString);
            stc->StyleSetForeground(wxSTC_ASM_CPUINSTRUCTION, cKeyword);
            stc->StyleSetBold(wxSTC_ASM_CPUINSTRUCTION, true);
            stc->StyleSetForeground(wxSTC_ASM_REGISTER, cNumber);
            stc->StyleSetForeground(wxSTC_ASM_DIRECTIVE, cDirect);
            break;
        case Lang::Basic:
            stc->SetLexer(wxSTC_LEX_VB);
            stc->SetKeyWords(0, kBasicKeywords);
            stc->StyleSetForeground(wxSTC_B_COMMENT, cComment);
            stc->StyleSetForeground(wxSTC_B_NUMBER, cNumber);
            stc->StyleSetForeground(wxSTC_B_STRING, cString);
            stc->StyleSetForeground(wxSTC_B_KEYWORD, cKeyword);
            stc->StyleSetBold(wxSTC_B_KEYWORD, true);
            break;
        case Lang::C:
            stc->SetLexer(wxSTC_LEX_CPP);
            stc->SetKeyWords(0, kCKeywords);
            stc->StyleSetForeground(wxSTC_C_COMMENT, cComment);
            stc->StyleSetForeground(wxSTC_C_COMMENTLINE, cComment);
            stc->StyleSetForeground(wxSTC_C_COMMENTDOC, cComment);
            stc->StyleSetForeground(wxSTC_C_NUMBER, cNumber);
            stc->StyleSetForeground(wxSTC_C_STRING, cString);
            stc->StyleSetForeground(wxSTC_C_CHARACTER, cString);
            stc->StyleSetForeground(wxSTC_C_WORD, cKeyword);
            stc->StyleSetBold(wxSTC_C_WORD, true);
            stc->StyleSetForeground(wxSTC_C_PREPROCESSOR, cDirect);
            break;
        case Lang::Text:
            stc->SetLexer(wxSTC_LEX_NULL);
            break;
    }

    // Line numbers, then a clickable symbol margin for breakpoints / execution
    stc->SetMarginType(kMarginLines, wxSTC_MARGIN_NUMBER);
    stc->SetMarginWidth(kMarginLines, stc->TextWidth(wxSTC_STYLE_LINENUMBER, "_99999"));
    stc->SetMarginType(kMarginSymbols, wxSTC_MARGIN_SYMBOL);
    stc->SetMarginWidth(kMarginSymbols, 16);
    stc->SetMarginSensitive(kMarginSymbols, true);
    stc->SetMarginMask(kMarginSymbols, (1 << kMarkBreakpoint) | (1 << kMarkExecArrow));
    stc->SetMarginMask(kMarginLines, 0);

    stc->MarkerDefine(kMarkBreakpoint, wxSTC_MARK_CIRCLE, wxColour(0x90, 0x10, 0x10), wxColour(0xE0, 0x30, 0x30));
    stc->MarkerDefine(kMarkExecArrow, wxSTC_MARK_SHORTARROW, wxColour(0x80, 0x60, 0x00), wxColour(0xFF, 0xD0, 0x30));
    stc->MarkerDefine(kMarkExecLine, wxSTC_MARK_BACKGROUND, wxNullColour,
                      dark ? wxColour(0x4A, 0x42, 0x10) : wxColour(0xFF, 0xF3, 0xB0));
}

void SourcePane::updateTabTitle(Doc& doc) {
    int idx = m_book->GetPageIndex(doc.stc);
    if (idx == wxNOT_FOUND) return;
    wxString title = wxString::FromUTF8(std::filesystem::path(doc.path).filename().string());
    if (doc.stc->GetModify()) title += " *";
    m_book->SetPageText(idx, title);
}

bool SourcePane::saveDoc(Doc& doc) {
    if (!doc.stc->SaveFile(wxString::FromUTF8(doc.path))) {
        if (m_status) m_status("Could not save " + wxString::FromUTF8(doc.path));
        return false;
    }
    updateTabTitle(doc);
    return true;
}

bool SourcePane::SaveCurrent() {
    std::string path = CurrentFile();
    Doc* doc = path.empty() ? nullptr : findDoc(path);
    if (!doc) return false;
    bool ok = saveDoc(*doc);
    if (ok && m_status) m_status("Saved " + wxString::FromUTF8(path));
    return ok;
}

bool SourcePane::SaveAll() {
    bool ok = true;
    for (auto& d : m_docs)
        if (d.stc->GetModify()) ok = saveDoc(d) && ok;
    return ok;
}

bool SourcePane::HasUnsavedChanges() const {
    for (const auto& d : m_docs) if (d.stc->GetModify()) return true;
    return false;
}

void SourcePane::RefreshBreakpointMarkers(bool force) {
    std::string sig = std::to_string(m_docs.size()) + (m_dbg ? "" : "-");
    if (m_dbg)
        for (const auto& bp : m_dbg->breakpoints().breakpoints())
            if (bp.type == BreakpointType::EXEC && bp.enabled) sig += ";" + std::to_string(bp.addr);
    if (!force && sig == m_bpSignature) return;
    m_bpSignature = sig;

    for (auto& d : m_docs) d.stc->MarkerDeleteAll(kMarkBreakpoint);
    if (!m_dbg) return;
    for (const auto& bp : m_dbg->breakpoints().breakpoints()) {
        if (bp.type != BreakpointType::EXEC || !bp.enabled) continue;
        SourceLocation loc = m_dbg->sourceMap().addrToSource(bp.addr);
        if (loc.file.empty() || loc.line < 1) continue;
        if (Doc* d = findDoc(loc.file)) d->stc->MarkerAdd(loc.line - 1, kMarkBreakpoint);
    }
}

void SourcePane::onMarginClick(wxStyledTextEvent& e) {
    auto* stc = dynamic_cast<wxStyledTextCtrl*>(e.GetEventObject());
    Doc* doc = stc ? docForCtrl(stc) : nullptr;
    if (!doc || !m_dbg) return;
    int line = stc->LineFromPosition(e.GetPosition()) + 1;
    uint32_t addr = 0;
    switch (source_stepper::toggleLineBreakpoint(*m_dbg, doc->path, line, &addr)) {
        case source_stepper::LineBreakpoint::NoCode:
            if (m_status) m_status(wxString::Format("No code at line %d (load a source map for this file)", line));
            return;
        case source_stepper::LineBreakpoint::Removed:
            if (m_status) m_status(wxString::Format("Breakpoint removed at $%04X (line %d)", addr, line));
            break;
        case source_stepper::LineBreakpoint::Added:
            if (m_status) m_status(wxString::Format("Breakpoint set at $%04X (line %d)", addr, line));
            break;
    }
    RefreshBreakpointMarkers(true);
    if (m_bpChanged) m_bpChanged();
}

void SourcePane::ClearExecutionLine() {
    if (Doc* d = findDoc(m_execFile)) {
        d->stc->MarkerDeleteAll(kMarkExecArrow);
        d->stc->MarkerDeleteAll(kMarkExecLine);
    }
    m_execFile.clear();
}

bool SourcePane::ShowExecutionLine(uint32_t pc) {
    ClearExecutionLine();
    if (!m_dbg) return false;
    SourceLocation loc = m_dbg->sourceMap().addrToSource(pc);
    if (loc.file.empty() || loc.line < 1) return false;
    if (!findDoc(loc.file) && (!std::filesystem::exists(loc.file) || !OpenFile(loc.file))) return false;
    Doc* d = findDoc(loc.file);
    if (!d) return false;
    m_book->SetSelection(m_book->GetPageIndex(d->stc));
    d->stc->MarkerAdd(loc.line - 1, kMarkExecArrow);
    d->stc->MarkerAdd(loc.line - 1, kMarkExecLine);
    d->stc->EnsureVisibleEnforcePolicy(loc.line - 1);
    d->stc->GotoLine(loc.line - 1);
    m_execFile = loc.file;
    return true;
}
