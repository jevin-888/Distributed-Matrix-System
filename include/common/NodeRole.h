#pragma once

#include <string>

namespace dms {

enum class NodeRole {
    UNASSIGNED,
    ENCODE,
    DECODE,
    CODEC,
};

inline const char* nodeRoleName(NodeRole role) {
    switch (role) {
        case NodeRole::UNASSIGNED: return "unassigned";
        case NodeRole::ENCODE: return "encode";
        case NodeRole::DECODE: return "decode";
        case NodeRole::CODEC: return "codec";
    }
    return "unassigned";
}

inline const char* nodeRoleOledLabel(NodeRole role) {
    switch (role) {
        case NodeRole::UNASSIGNED: return "--";
        case NodeRole::ENCODE: return "IN";
        case NodeRole::DECODE: return "OUT";
        case NodeRole::CODEC: return "IN/OUT";
    }
    return "--";
}

inline bool parseNodeRole(const std::string& value, NodeRole& role) {
    if (value == "unassigned") role = NodeRole::UNASSIGNED;
    else if (value == "encode") role = NodeRole::ENCODE;
    else if (value == "decode") role = NodeRole::DECODE;
    else if (value == "codec") role = NodeRole::CODEC;
    else return false;
    return true;
}

} // namespace dms
