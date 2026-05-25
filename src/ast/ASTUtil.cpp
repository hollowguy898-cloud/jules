#include "ast/AST.h"

namespace jules {

std::string binaryOpToString(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:       return "+";
        case BinaryOp::Sub:       return "-";
        case BinaryOp::Mul:       return "*";
        case BinaryOp::Div:       return "/";
        case BinaryOp::Mod:       return "%";
        case BinaryOp::And:       return "&&";
        case BinaryOp::Or:        return "||";
        case BinaryOp::BitAnd:    return "&";
        case BinaryOp::BitOr:     return "|";
        case BinaryOp::BitXor:    return "^";
        case BinaryOp::Shl:       return "<<";
        case BinaryOp::Shr:       return ">>";
        case BinaryOp::Eq:        return "==";
        case BinaryOp::Ne:        return "!=";
        case BinaryOp::Lt:        return "<";
        case BinaryOp::Le:        return "<=";
        case BinaryOp::Gt:        return ">";
        case BinaryOp::Ge:        return ">=";
        case BinaryOp::Assign:    return "=";
        case BinaryOp::AddAssign: return "+=";
        case BinaryOp::SubAssign: return "-=";
        case BinaryOp::MulAssign: return "*=";
        case BinaryOp::DivAssign: return "/=";
        case BinaryOp::ModAssign: return "%=";
        case BinaryOp::AndAssign: return "&=";
        case BinaryOp::OrAssign:  return "|=";
        case BinaryOp::XorAssign: return "^=";
        case BinaryOp::ShlAssign: return "<<=";
        case BinaryOp::ShrAssign: return ">>=";
        default:                  return "unknown_binary_op";
    }
}

std::string unaryOpToString(UnaryOp op) {
    switch (op) {
        case UnaryOp::Neg:    return "-";
        case UnaryOp::Not:    return "!";
        case UnaryOp::BitNot: return "~";
        case UnaryOp::Deref:  return "*";
        case UnaryOp::Addr:   return "&";
        default:              return "unknown_unary_op";
    }
}

std::string compilerDirectiveToString(CompilerDirective d) {
    switch (d) {
        case CompilerDirective::Superoptimize: return "@superoptimize";
        case CompilerDirective::Polly:         return "@polly";
        default:                               return "@unknown";
    }
}

} // namespace jules
