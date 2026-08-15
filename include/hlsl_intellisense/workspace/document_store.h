#pragma once

#include <hlsl_intellisense/workspace/document_uri.h>
#include <hlsl_intellisense/workspace/text_position.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hlsl_intellisense::workspace {

struct ContentChange {
    std::optional<Range> range;
    std::optional<std::size_t> range_length;
    std::string text;
};

class SourceSnapshot {
  public:
    SourceSnapshot(DocumentUri document_uri, std::string language_id, std::int64_t version,
                   std::string text);

    [[nodiscard]] const DocumentUri& document_uri() const noexcept;
    [[nodiscard]] const std::string& uri() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::string& language_id() const noexcept;
    [[nodiscard]] std::int64_t version() const noexcept;
    [[nodiscard]] const std::string& text() const noexcept;

  private:
    DocumentUri document_uri_;
    std::string language_id_;
    std::int64_t version_;
    std::string text_;
};

struct DocumentState {
    DocumentUri document_uri;
    std::string language_id;
    std::int64_t version{};
    std::string text;
    bool open{};
    bool dirty{};
};

class DocumentStore {
  public:
    explicit DocumentStore(PathStyle path_style = PathStyle::native);

    void did_open(std::string_view uri, std::string language_id, std::int64_t version,
                  std::string text);
    void did_change(std::string_view uri, std::int64_t version,
                    std::span<const ContentChange> changes);
    void did_close(std::string_view uri);
    void did_save(std::string_view uri, std::optional<std::string> text = std::nullopt);

    [[nodiscard]] bool contains(std::string_view uri) const;
    [[nodiscard]] const DocumentState& document(std::string_view uri) const;
    [[nodiscard]] SourceSnapshot snapshot(std::string_view uri) const;
    [[nodiscard]] std::vector<SourceSnapshot> open_snapshots() const;

  private:
    [[nodiscard]] DocumentUri normalize(std::string_view uri) const;
    [[nodiscard]] DocumentState& open_document(std::string_view uri);
    [[nodiscard]] const DocumentState& find_document(std::string_view uri) const;

    PathStyle path_style_;
    std::unordered_map<std::string, DocumentState> documents_;
};

} // namespace hlsl_intellisense::workspace
