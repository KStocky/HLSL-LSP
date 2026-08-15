#include <hlsl_intellisense/workspace/document_store.h>

#include <utility>

namespace hlsl_intellisense::workspace {

SourceSnapshot::SourceSnapshot(DocumentUri document_uri, std::string language_id,
                               std::int64_t version, std::string text)
    : document_uri_{std::move(document_uri)}, language_id_{std::move(language_id)},
      version_{version}, text_{std::move(text)} {}

const DocumentUri& SourceSnapshot::document_uri() const noexcept { return document_uri_; }

const std::string& SourceSnapshot::uri() const noexcept { return document_uri_.uri(); }

const std::string& SourceSnapshot::path() const noexcept { return document_uri_.path(); }

const std::string& SourceSnapshot::language_id() const noexcept { return language_id_; }

std::int64_t SourceSnapshot::version() const noexcept { return version_; }

const std::string& SourceSnapshot::text() const noexcept { return text_; }

DocumentStore::DocumentStore(PathStyle path_style) : path_style_{path_style} {}

void DocumentStore::did_open(std::string_view uri, std::string language_id, std::int64_t version,
                             std::string text) {
    static_cast<void>(utf16_length(text));
    auto document_uri = normalize(uri);
    const auto existing = documents_.find(document_uri.identity());
    if (existing != documents_.end() && existing->second.open) {
        throw DocumentError{DocumentErrorCode::duplicate_open, "Document is already open"};
    }

    auto state = DocumentState{.document_uri = std::move(document_uri),
                               .language_id = std::move(language_id),
                               .version = version,
                               .text = std::move(text),
                               .open = true,
                               .dirty = false};
    documents_.insert_or_assign(state.document_uri.identity(), std::move(state));
}

void DocumentStore::did_change(std::string_view uri, std::int64_t version,
                               std::span<const ContentChange> changes) {
    auto& state = open_document(uri);
    if (version <= state.version) {
        throw DocumentError{DocumentErrorCode::version_not_increasing,
                            "Document version must increase"};
    }

    auto updated_text = state.text;
    for (const auto& change : changes) {
        if (!change.range.has_value()) {
            if (change.range_length.has_value()) {
                throw DocumentError{DocumentErrorCode::invalid_range,
                                    "A full change cannot specify rangeLength"};
            }
            static_cast<void>(utf16_length(change.text));
            updated_text = change.text;
            continue;
        }

        const auto start = utf8_offset_at(updated_text, change.range->start);
        const auto end = utf8_offset_at(updated_text, change.range->end);
        if (start > end) {
            throw DocumentError{DocumentErrorCode::invalid_range,
                                "Change range starts after it ends"};
        }
        if (change.range_length.has_value() &&
            *change.range_length !=
                utf16_length(std::string_view{updated_text}.substr(start, end - start))) {
            throw DocumentError{DocumentErrorCode::range_length_mismatch,
                                "rangeLength does not match the replaced text"};
        }
        static_cast<void>(utf16_length(change.text));
        updated_text.replace(start, end - start, change.text);
    }

    state.text = std::move(updated_text);
    state.version = version;
    state.dirty = true;
}

void DocumentStore::did_close(std::string_view uri) {
    auto& state = open_document(uri);
    state.open = false;
    state.dirty = false;
}

void DocumentStore::did_save(std::string_view uri, std::optional<std::string> text) {
    auto& state = open_document(uri);
    if (text.has_value()) {
        static_cast<void>(utf16_length(*text));
        state.text = std::move(*text);
    }
    state.dirty = false;
}

bool DocumentStore::contains(std::string_view uri) const {
    const auto document_uri = normalize(uri);
    return documents_.contains(document_uri.identity());
}

const DocumentState& DocumentStore::document(std::string_view uri) const {
    return find_document(uri);
}

SourceSnapshot DocumentStore::snapshot(std::string_view uri) const {
    const auto& state = find_document(uri);
    return SourceSnapshot{state.document_uri, state.language_id, state.version, state.text};
}

DocumentUri DocumentStore::normalize(std::string_view uri) const {
    return DocumentUri::from_uri(uri, path_style_);
}

DocumentState& DocumentStore::open_document(std::string_view uri) {
    auto document_uri = normalize(uri);
    const auto existing = documents_.find(document_uri.identity());
    if (existing == documents_.end()) {
        throw DocumentError{DocumentErrorCode::document_not_found, "Document is not in the store"};
    }
    if (!existing->second.open) {
        throw DocumentError{DocumentErrorCode::document_not_open, "Document is not open"};
    }
    return existing->second;
}

const DocumentState& DocumentStore::find_document(std::string_view uri) const {
    auto document_uri = normalize(uri);
    const auto existing = documents_.find(document_uri.identity());
    if (existing == documents_.end()) {
        throw DocumentError{DocumentErrorCode::document_not_found, "Document is not in the store"};
    }
    return existing->second;
}

} // namespace hlsl_intellisense::workspace
