// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "message_handler.h"
#include "pipeline.hh"
#include "project.h"
#include "query_utils.h"
#include "working_files.h"

namespace ccls {
namespace {
MethodType foldingRange = "textDocument/foldingRange";

struct In_textDocumentFoldingRange : public RequestMessage {
  MethodType GetMethodType() const override { return foldingRange; }
  struct Params {
    lsTextDocumentIdentifier textDocument;
  } params;
};
MAKE_REFLECT_STRUCT(In_textDocumentFoldingRange::Params, textDocument);
MAKE_REFLECT_STRUCT(In_textDocumentFoldingRange, id, params);
REGISTER_IN_MESSAGE(In_textDocumentFoldingRange);

struct FoldingRange {
  int startLine, startCharacter, endLine, endCharacter;
  std::string kind = "region";
};
MAKE_REFLECT_STRUCT(FoldingRange, startLine, startCharacter, endLine, endCharacter, kind);

struct Handler_textDocumentFoldingRange
    : BaseMessageHandler<In_textDocumentFoldingRange> {
  MethodType GetMethodType() const override { return foldingRange; }
  void Run(In_textDocumentFoldingRange *request) override {
    QueryFile *file;
    if (!FindFileOrFail(db, project, request->id,
                        request->params.textDocument.uri.GetPath(), &file,
                        nullptr))
      return;
    WorkingFile *wfile =
        working_files->GetFileByFilename(file->def->path);
    if (!wfile)
      return;
    std::vector<FoldingRange> result;
    std::optional<lsRange> ls_range;

    for (auto [sym, refcnt] : file->symbol2refcnt)
      if (refcnt > 0 && sym.extent.Valid() &&
          (sym.kind == SymbolKind::Func || sym.kind == SymbolKind::Type) &&
          (ls_range = GetLsRange(wfile, sym.extent))) {
        FoldingRange &fold = result.emplace_back();
        fold.startLine = ls_range->start.line;
        fold.startCharacter = ls_range->start.character;
        fold.endLine = ls_range->end.line;
        fold.endCharacter = ls_range->end.character;
      }
    pipeline::Reply(request->id, result);
  }
};
REGISTER_MESSAGE_HANDLER(Handler_textDocumentFoldingRange);
} // namespace
} // namespace ccls