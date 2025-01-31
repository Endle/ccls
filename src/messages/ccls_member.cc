// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "clang_tu.hh"
#include "hierarchy.hh"
#include "message_handler.hh"
#include "pipeline.hh"
#include "query.hh"

#include <clang/AST/Type.h>
#include <llvm/ADT/DenseSet.h>

#include <unordered_set>

namespace ccls {
using namespace clang;

namespace {
struct Param : TextDocumentPositionParam {
  // If id is specified, expand a node; otherwise textDocument+position should
  // be specified for building the root and |levels| of nodes below.
  Usr usr;
  std::string id;

  bool qualified = false;
  int levels = 1;
  // If Kind::Func and the point is at a type, list member functions instead of
  // member variables.
  Kind kind = Kind::Var;
  bool hierarchy = false;
};

REFLECT_STRUCT(Param, textDocument, position, id, qualified, levels, kind, hierarchy);

struct Out_cclsMember {
  Usr usr;
  std::string id;
  std::string_view name;
  std::string fieldName;
  Location location;
  // For unexpanded nodes, this is an upper bound because some entities may be
  // undefined. If it is 0, there are no members.
  int numChildren = 0;
  // Empty if the |levels| limit is reached.
  std::vector<Out_cclsMember> children;
};
REFLECT_STRUCT(Out_cclsMember, id, name, fieldName, location, numChildren, children);

bool expand(MessageHandler *m, Out_cclsMember *entry, bool qualified, int levels, Kind memberKind);

// Add a field to |entry| which is a Func/Type.
void doField(MessageHandler *m, Out_cclsMember *entry, const QueryVar &var, int64_t offset, bool qualified,
             int levels) {
  const QueryVar::Def *def1 = var.anyDef();
  if (!def1)
    return;
  Out_cclsMember entry1;
  // With multiple inheritance, the offset is incorrect.
  if (offset >= 0) {
    if (offset / 8 < 10)
      entry1.fieldName += ' ';
    entry1.fieldName += std::to_string(offset / 8);
    if (offset % 8) {
      entry1.fieldName += '.';
      entry1.fieldName += std::to_string(offset % 8);
    }
    entry1.fieldName += ' ';
  }
  if (qualified)
    entry1.fieldName += def1->detailed_name;
  else {
    entry1.fieldName += std::string_view(def1->detailed_name).substr(0, def1->qual_name_offset);
    entry1.fieldName += def1->name(false);
  }
  if (def1->spell) {
    if (std::optional<Location> loc = getLsLocation(m->db, m->wfiles, *def1->spell))
      entry1.location = *loc;
  }
  if (def1->type) {
    entry1.id = std::to_string(def1->type);
    entry1.usr = def1->type;
    if (expand(m, &entry1, qualified, levels, Kind::Var))
      entry->children.push_back(std::move(entry1));
  } else {
    entry1.id = "0";
    entry1.usr = 0;
    entry->children.push_back(std::move(entry1));
  }
}

// Expand a type node by adding members recursively to it.
bool expand(MessageHandler *m, Out_cclsMember *entry, bool qualified, int levels, Kind memberKind) {
  if (0 < entry->usr && entry->usr <= BuiltinType::LastKind) {
    entry->name = clangBuiltinTypeName(int(entry->usr));
    return true;
  }
  const QueryType *type = &m->db->getType(entry->usr);
  const QueryType::Def *def = type->anyDef();
  // builtin types have no declaration and empty |qualified|.
  if (!def)
    return false;
  entry->name = def->name(qualified);
  std::unordered_set<Usr> seen;
  if (levels > 0) {
    std::vector<const QueryType *> stack;
    seen.insert(type->usr);
    stack.push_back(type);
    while (stack.size()) {
      type = stack.back();
      stack.pop_back();
      const auto *def = type->anyDef();
      if (!def)
        continue;
      if (def->kind != SymbolKind::Namespace)
        for (Usr usr : def->bases) {
          auto &type1 = m->db->getType(usr);
          if (type1.def.size()) {
            seen.insert(type1.usr);
            stack.push_back(&type1);
          }
        }
      if (def->alias_of) {
        const QueryType::Def *def1 = m->db->getType(def->alias_of).anyDef();
        Out_cclsMember entry1;
        entry1.id = std::to_string(def->alias_of);
        entry1.usr = def->alias_of;
        if (def1 && def1->spell) {
          // The declaration of target type.
          if (std::optional<Location> loc = getLsLocation(m->db, m->wfiles, *def1->spell))
            entry1.location = *loc;
        } else if (def->spell) {
          // Builtin types have no declaration but the typedef declaration
          // itself is useful.
          if (std::optional<Location> loc = getLsLocation(m->db, m->wfiles, *def->spell))
            entry1.location = *loc;
        }
        if (def1 && qualified)
          entry1.fieldName = def1->detailed_name;
        if (expand(m, &entry1, qualified, levels - 1, memberKind)) {
          // For builtin types |name| is set.
          if (entry1.fieldName.empty())
            entry1.fieldName = std::string(entry1.name);
          entry->children.push_back(std::move(entry1));
        }
      } else if (memberKind == Kind::Func) {
        llvm::DenseSet<Usr, DenseMapInfoForUsr> seen1;
        for (auto &def : type->def)
          for (Usr usr : def.funcs)
            if (seen1.insert(usr).second) {
              QueryFunc &func1 = m->db->getFunc(usr);
              if (const QueryFunc::Def *def1 = func1.anyDef()) {
                Out_cclsMember entry1;
                entry1.fieldName = def1->name(false);
                if (def1->spell) {
                  if (auto loc = getLsLocation(m->db, m->wfiles, *def1->spell))
                    entry1.location = *loc;
                } else if (func1.declarations.size()) {
                  if (auto loc = getLsLocation(m->db, m->wfiles, func1.declarations[0]))
                    entry1.location = *loc;
                }
                entry->children.push_back(std::move(entry1));
              }
            }
      } else if (memberKind == Kind::Type) {
        llvm::DenseSet<Usr, DenseMapInfoForUsr> seen1;
        for (auto &def : type->def)
          for (Usr usr : def.types)
            if (seen1.insert(usr).second) {
              QueryType &type1 = m->db->getType(usr);
              if (const QueryType::Def *def1 = type1.anyDef()) {
                Out_cclsMember entry1;
                entry1.fieldName = def1->name(false);
                if (def1->spell) {
                  if (auto loc = getLsLocation(m->db, m->wfiles, *def1->spell))
                    entry1.location = *loc;
                } else if (type1.declarations.size()) {
                  if (auto loc = getLsLocation(m->db, m->wfiles, type1.declarations[0]))
                    entry1.location = *loc;
                }
                entry->children.push_back(std::move(entry1));
              }
            }
      } else {
        llvm::DenseSet<Usr, DenseMapInfoForUsr> seen1;
        for (auto &def : type->def)
          for (auto it : def.vars)
            if (seen1.insert(it.first).second) {
              QueryVar &var = m->db->getVar(it.first);
              if (!var.def.empty())
                doField(m, entry, var, it.second, qualified, levels - 1);
            }
      }
    }
    entry->numChildren = int(entry->children.size());
  } else
    entry->numChildren = def->alias_of ? 1 : int(def->vars.size());
  return true;
}

std::optional<Out_cclsMember> buildInitial(MessageHandler *m, Kind kind, Usr root_usr, bool qualified, int levels,
                                           Kind memberKind) {
  switch (kind) {
  default:
    return {};
  case Kind::Func: {
    const auto *def = m->db->getFunc(root_usr).anyDef();
    if (!def)
      return {};

    Out_cclsMember entry;
    // Not type, |id| is invalid.
    entry.name = def->name(qualified);
    if (def->spell) {
      if (auto loc = getLsLocation(m->db, m->wfiles, *def->spell))
        entry.location = *loc;
    }
    for (Usr usr : def->vars) {
      auto &var = m->db->getVar(usr);
      if (var.def.size())
        doField(m, &entry, var, -1, qualified, levels - 1);
    }
    return entry;
  }
  case Kind::Type: {
    const auto *def = m->db->getType(root_usr).anyDef();
    if (!def)
      return {};

    Out_cclsMember entry;
    entry.id = std::to_string(root_usr);
    entry.usr = root_usr;
    if (def->spell) {
      if (auto loc = getLsLocation(m->db, m->wfiles, *def->spell))
        entry.location = *loc;
    }
    expand(m, &entry, qualified, levels, memberKind);
    return entry;
  }
  }
}
} // namespace

void MessageHandler::ccls_member(JsonReader &reader, ReplyOnce &reply) {
  Param param;
  reflect(reader, param);
  std::optional<Out_cclsMember> result;
  if (param.id.size()) {
    try {
      param.usr = std::stoull(param.id);
    } catch (...) {
      return;
    }
    result.emplace();
    result->id = std::to_string(param.usr);
    result->usr = param.usr;
    // entry.name is empty as it is known by the client.
    if (!(db->hasType(param.usr) && expand(this, &*result, param.qualified, param.levels, param.kind)))
      result.reset();
  } else {
    auto [file, wf] = findOrFail(param.textDocument.uri.getPath(), reply);
    if (!wf)
      return;
    for (SymbolRef sym : findSymbolsAtLocation(wf, file, param.position)) {
      switch (sym.kind) {
      case Kind::Func:
      case Kind::Type:
        result = buildInitial(this, sym.kind, sym.usr, param.qualified, param.levels, param.kind);
        break;
      case Kind::Var: {
        const QueryVar::Def *def = db->getVar(sym).anyDef();
        if (def && def->type)
          result = buildInitial(this, Kind::Type, def->type, param.qualified, param.levels, param.kind);
        break;
      }
      default:
        continue;
      }
      break;
    }
  }

  if (param.hierarchy)
    reply(result);
  else
    reply(flattenHierarchy(result));
}
} // namespace ccls
