#include "expression_evaluator.h"
#include "debug_context.h"
#include "libcore/main/icore.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

static std::string trim(const std::string& s) {
    auto first = std::find_if_not(s.begin(), s.end(), [](unsigned char ch) { return std::isspace(ch); });
    auto last  = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
    return (first < last) ? std::string(first, last) : "";
}

static bool removeOuterParentheses(std::string& expr) {
    if (expr.empty() || expr[0] != '(' || expr.back() != ')') return false;
    
    int depth = 0;
    for (size_t i = 0; i < expr.length() - 1; ++i) {
        if (expr[i] == '(') depth++;
        else if (expr[i] == ')') depth--;
        if (depth == 0) return false;
    }
    
    expr = trim(expr.substr(1, expr.length() - 2));
    return true;
}

bool ExpressionEvaluator::evaluate(const std::string& expression, DebugContext* dbg, uint32_t& result, int defaultBase) {
    double res;
    if (evaluate(expression, dbg, res, defaultBase)) {
        result = (uint32_t)res;
        return true;
    }
    return false;
}

bool ExpressionEvaluator::evaluate(const std::string& expression, DebugContext* dbg, double& result, int defaultBase) {
    std::string expr = trim(expression);
    while (removeOuterParentheses(expr));
    if (expr.empty()) return false;

    struct OpInfo { std::string s; int precedence; };
    static const std::vector<OpInfo> binOps = {
        {"||", 0},
        {"&&", 0},
        {"==", 1}, {"!=", 1}, {"<=", 1}, {">=", 1}, {"<",  1}, {">",  1},
        {"|",  2},
        {"&",  3}, {"<<", 3}, {">>", 3},
        {"+",  4}, {"-",  4},
        {"*",  5}, {"/",  5}, {"%",  5},
        {"^",  6}
    };

    size_t opPos = std::string::npos;
    std::string opStr;
    int bestPrec = 1000;

    int depth = 0;
    for (int i = 0; i < (int)expr.length(); ++i) {
        if (expr[i] == '(') depth++;
        else if (expr[i] == ')') depth--;
        else if (depth == 0) {
            std::string foundOp;
            int foundPrec = -1;
            
            for (const auto& op : binOps) {
                if (i + op.s.length() <= expr.length() && expr.substr(i, op.s.length()) == op.s) {
                    // Pre-check for modulus vs binary literal
                    if (op.s == "%" && (size_t)i + 1 < expr.length()) {
                        char next = expr[i + 1];
                        if (next == '0' || next == '1') continue;
                    }

                    // Check if it's a binary op (needs something on the left)
                    bool hasLeft = false;
                    for (int j = i - 1; j >= 0; --j) {
                        if (!std::isspace((unsigned char)expr[j])) {
                            hasLeft = true;
                            break;
                        }
                    }
                    if (!hasLeft) continue;

                    // Greedily pick the longest matching operator at this position
                    if (op.s.length() > foundOp.length()) {
                        foundOp = op.s;
                        foundPrec = op.precedence;
                    }
                }
            }

            if (!foundOp.empty()) {
                if (foundPrec <= bestPrec) {
                    bestPrec = foundPrec;
                    opPos = i;
                    opStr = foundOp;
                }
                i += foundOp.length() - 1;
            }
        }
    }

    if (opPos != std::string::npos) {
        double a, b;
        if (evaluate(expr.substr(0, opPos), dbg, a, defaultBase) && evaluate(expr.substr(opPos + opStr.length()), dbg, b, defaultBase)) {
            if (opStr == "||") result = (double)(a != 0 || b != 0);
            else if (opStr == "&&") result = (double)(a != 0 && b != 0);
            else if (opStr == "==") result = (double)(a == b);
            else if (opStr == "!=") result = (double)(a != b);
            else if (opStr == "<=") result = (double)(a <= b);
            else if (opStr == ">=") result = (double)(a >= b);
            else if (opStr == "<")  result = (double)(a < b);
            else if (opStr == ">")  result = (double)(a > b);
            else if (opStr == "|")  result = (double)((long long)a | (long long)b);
            else if (opStr == "&")  result = (double)((long long)a & (long long)b);
            else if (opStr == "<<") result = (double)((long long)a << (long long)b);
            else if (opStr == ">>") result = (double)((long long)a >> (long long)b);
            else if (opStr == "+")  result = a + b;
            else if (opStr == "-")  result = a - b;
            else if (opStr == "*")  result = a * b;
            else if (opStr == "/") {
                if (b == 0) return false;
                result = a / b;
            } else if (opStr == "%") {
                if (b == 0) return false;
                result = (double)((long long)a % (long long)b);
            } else if (opStr == "^") {
                result = std::pow(a, b);
            }
            return true;
        }
        return false;
    }

    // Unary prefix operators
    if (expr[0] == '-') {
        double a;
        if (evaluate(expr.substr(1), dbg, a, defaultBase)) {
            result = -a;
            return true;
        }
        return false;
    }
    if (expr[0] == '!') {
        double a;
        if (evaluate(expr.substr(1), dbg, a, defaultBase)) {
            result = (a == 0) ? 1.0 : 0.0;
            return true;
        }
        return false;
    }
    if (expr[0] == '<' && (expr.length() == 1 || expr[1] != '<')) {
        double a;
        if (evaluate(expr.substr(1), dbg, a, defaultBase)) {
            result = (double)(((uint32_t)a >> 8) & 0xFF);
            return true;
        }
        return false;
    }
    if (expr[0] == '>' && (expr.length() == 1 || expr[1] != '>')) {
        double a;
        if (evaluate(expr.substr(1), dbg, a, defaultBase)) {
            result = (double)((uint32_t)a & 0xFF);
            return true;
        }
        return false;
    }
    // Memory dereference: *addr reads byte, *addr:16 reads 16-bit LE word
    if (expr[0] == '*' && expr.length() > 1) {
        // Check for :16 suffix
        bool word = false;
        std::string inner = expr.substr(1);
        if (inner.length() > 3 && inner.substr(inner.length() - 3) == ":16") {
            word = true;
            inner = inner.substr(0, inner.length() - 3);
        }
        double a;
        if (dbg && dbg->bus() && evaluate(inner, dbg, a, defaultBase)) {
            uint32_t addr = (uint32_t)a;
            if (word) {
                result = (double)(dbg->bus()->peek8(addr) | (dbg->bus()->peek8(addr + 1) << 8));
            } else {
                result = (double)dbg->bus()->peek8(addr);
            }
            return true;
        }
        return false;
    }

    // Functions
    auto handleFunc = [&](const std::string& name, double (*func)(double)) {
        if (expr.length() > name.length() + 1 && expr.substr(0, name.length()) == name && expr[name.length()] == '(') {
            double a;
            if (expr.back() == ')' && evaluate(expr.substr(name.length() + 1, expr.length() - name.length() - 2), dbg, a, defaultBase)) {
                result = func(a); return true;
            }
        }
        return false;
    };

    if (handleFunc("sin", std::sin)) return true;
    if (handleFunc("cos", std::cos)) return true;
    if (handleFunc("tan", std::tan)) return true;
    if (handleFunc("asin", std::asin)) return true;
    if (handleFunc("acos", std::acos)) return true;
    if (handleFunc("atan", std::atan)) return true;
    if (handleFunc("sqrt", std::sqrt)) return true;
    if (handleFunc("log", std::log10)) return true;
    if (handleFunc("exp", std::exp)) return true;
    if (handleFunc("abs", std::abs)) return true;

    // Multi-argument functions
    if (expr.length() > 8 && expr.substr(0, 8) == "between(" && expr.back() == ')') {
        std::string inner = expr.substr(8, expr.length() - 9);
        std::vector<std::string> args;
        int depth = 0;
        size_t last = 0;
        for (size_t i = 0; i < inner.length(); ++i) {
            if (inner[i] == '(') depth++;
            else if (inner[i] == ')') depth--;
            else if (inner[i] == ',' && depth == 0) {
                args.push_back(inner.substr(last, i - last));
                last = i + 1;
            }
        }
        args.push_back(inner.substr(last));
        
        if (args.size() == 3) {
            double v, target, delta;
            if (evaluate(args[0], dbg, v, defaultBase) && 
                evaluate(args[1], dbg, target, defaultBase) && 
                evaluate(args[2], dbg, delta, defaultBase)) {
                result = (v >= (target - delta) && v <= (target + delta)) ? 1.0 : 0.0;
                return true;
            }
        }
    }

    // Literals
    if (expr[0] == '$') {
        if (expr.length() < 2 || std::isspace((unsigned char)expr[1])) return false;
        std::string sub = expr.substr(1);
        try {
            size_t processed = 0;
            result = (double)std::stoull(sub, &processed, 16);
            return processed == sub.length();
        } catch (...) { return false; }
    } else if (expr[0] == '0' && expr.length() > 1 && (expr[1] == 'x' || expr[1] == 'X')) {
        std::string sub = expr.substr(2);
        try {
            size_t processed = 0;
            result = (double)std::stoull(sub, &processed, 16);
            return processed == sub.length();
        } catch (...) { return false; }
    } else if (expr[0] == '%') {
        if (expr.length() < 2 || std::isspace((unsigned char)expr[1])) return false;
        std::string sub = expr.substr(1);
        try {
            size_t processed = 0;
            result = (double)std::stoull(sub, &processed, 2);
            return processed == sub.length();
        } catch (...) { return false; }
    } else if (expr.length() >= 3 && expr.front() == '\'' && expr.back() == '\'') {
        if (expr.length() == 3) {
            result = (double)(unsigned char)expr[1];
            return true;
        } else if (expr.length() == 4 && expr[1] == '\\') {
            switch (expr[2]) {
                case 'n':  result = (double)'\n'; return true;
                case 'r':  result = (double)'\r'; return true;
                case 't':  result = (double)'\t'; return true;
                case '\\': result = (double)'\\'; return true;
                case '\'': result = (double)'\''; return true;
                default:   return false;
            }
        }
        return false;
    }

    // Try parsing as number with defaultBase if not 10
    if (defaultBase != 10) {
        try {
            size_t processed = 0;
            result = (double)std::stoull(expr, &processed, defaultBase);
            if (processed == expr.length()) return true;
        } catch (...) {}
    }

    // Decimal or floating point (base 10)
    if (std::isdigit(expr[0]) || (expr[0] == '.' && expr.length() > 1 && std::isdigit(expr[1]))) {
        try {
            size_t processed = 0;
            result = std::stod(expr, &processed);
            if (processed == expr.length()) return true;
        } catch (...) {}
    }

    // Status flag shorthand
    if (expr.size() == 2 && expr[0] == '.') {
        if (dbg && dbg->cpu()) {
            char flagLetter = (char)std::toupper((unsigned char)expr[1]);
            ICore* cpu = dbg->cpu();
            for (int i = 0; i < cpu->regCount(); ++i) {
                const RegDescriptor* d = cpu->regDescriptor(i);
                if (!(d->flags & REGFLAG_STATUS) || !d->flagNames) continue;
                int nbits = (d->width == RegWidth::R8) ? 8 : 16;
                for (int b = 0; b < nbits; ++b) {
                    if (std::toupper((unsigned char)d->flagNames[b]) == flagLetter) {
                        result = (double)((cpu->regRead(i) >> (nbits - 1 - b)) & 1);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // Register shorthand
    if (!expr.empty() && expr[0] == '@') {
        if (dbg && dbg->cpu() && expr.size() >= 2) {
            std::string regName = expr.substr(1);
            std::transform(regName.begin(), regName.end(), regName.begin(), ::toupper);
            int regIdx = dbg->cpu()->regIndexByName(regName.c_str());
            if (regIdx != -1) {
                result = (double)dbg->cpu()->regRead(regIdx);
                return true;
            }
        }
        return false;
    }

    if (dbg) {
        uint32_t symAddr = dbg->symbols().getAddress(expr);
        if (symAddr != 0xFFFFFFFF) {
            result = (double)symAddr;
            return true;
        }
        // Device derived values: "DEVICE.NAME" pattern (e.g. VIC4.SCREEN)
        auto dotPos = expr.find('.');
        if (dotPos != std::string::npos && dbg->ioRegistry()) {
            std::string devPart = expr.substr(0, dotPos);
            std::string valPart = expr.substr(dotPos + 1);
            std::transform(devPart.begin(), devPart.end(), devPart.begin(), ::toupper);
            std::transform(valPart.begin(), valPart.end(), valPart.begin(), ::toupper);
            auto* ioReg = static_cast<IORegistry*>(dbg->ioRegistry());
            std::vector<IOHandler*> handlers;
            ioReg->enumerate(handlers);
            for (auto* h : handlers) {
                // Match against device name and aliases
                bool match = false;
                std::string hn = h->name();
                std::string hnUpper = hn;
                std::transform(hnUpper.begin(), hnUpper.end(), hnUpper.begin(), ::toupper);
                if (hnUpper == devPart) match = true;
                if (!match) {
                    for (auto& alias : h->deviceAliases()) {
                        std::string au = alias;
                        std::transform(au.begin(), au.end(), au.begin(), ::toupper);
                        if (au == devPart) { match = true; break; }
                    }
                }
                if (match) {
                    auto vals = h->getDerivedValues();
                    for (auto& [name, val] : vals) {
                        std::string nameUpper = name;
                        std::transform(nameUpper.begin(), nameUpper.end(), nameUpper.begin(), ::toupper);
                        if (nameUpper == valPart) {
                            result = (double)val;
                            return true;
                        }
                    }
                }
            }
        }
        if (dbg->cpu()) {
            std::string upperName = expr;
            std::transform(upperName.begin(), upperName.end(), upperName.begin(), ::toupper);
            int regIdx = dbg->cpu()->regIndexByName(upperName.c_str());
            if (regIdx != -1) {
                result = (double)dbg->cpu()->regRead(regIdx);
                return true;
            }
        }
    }

    return false;
}

bool ExpressionEvaluator::evaluateCondition(const std::string& expression, DebugContext* dbg, int defaultBase) {
    if (expression.empty()) return true;
    double result;
    if (evaluate(expression, dbg, result, defaultBase)) {
        return result != 0;
    }
    return false;
}
