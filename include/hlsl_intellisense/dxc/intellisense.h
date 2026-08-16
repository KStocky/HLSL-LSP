#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hlsl_intellisense::dxc {

struct SourceFile {
    std::string path;
    std::string text;
};

struct CompilerOptions {
    std::string language_version{"2021"};
    std::string target_profile;
    std::string entry_point;
    std::vector<std::string> defines;
    std::vector<std::string> include_directories;
    std::vector<std::string> additional_arguments;

    [[nodiscard]] std::vector<std::string> arguments() const;
};

enum class DiagnosticSeverity : std::uint8_t { ignored, note, warning, error, fatal };

struct SourceLocation {
    std::string path;
    std::uint32_t line{};
    std::uint32_t column{};
    std::uint32_t offset{};
};

struct Diagnostic {
    DiagnosticSeverity severity{};
    std::string message;
    SourceLocation location;
};

struct Completion {
    std::string label;
    std::string detail;
    std::uint32_t cursor_kind{};
};

struct Definition {
    std::string name;
    SourceLocation location;
};

enum class TokenKind : std::uint8_t {
    punctuation,
    keyword,
    identifier,
    literal,
    comment,
    unknown,
    built_in_type
};

struct Token {
    std::uint32_t line{};
    std::uint32_t column{};
    std::uint32_t length{};
    TokenKind kind{TokenKind::unknown};
    std::uint32_t cursor_kind{};
};

class TranslationUnit final {
  public:
    TranslationUnit(TranslationUnit&&) noexcept;
    TranslationUnit& operator=(TranslationUnit&&) noexcept;
    TranslationUnit(const TranslationUnit&) = delete;
    TranslationUnit& operator=(const TranslationUnit&) = delete;
    ~TranslationUnit();

    [[nodiscard]] std::vector<Diagnostic> diagnostics() const;
    [[nodiscard]] std::vector<Completion> complete(std::string_view path, std::uint32_t line,
                                                   std::uint32_t column) const;
    [[nodiscard]] std::optional<Definition> definition_at(std::string_view path, std::uint32_t line,
                                                          std::uint32_t column) const;
    [[nodiscard]] std::vector<Token> tokens(std::string_view path) const;

    void reparse(std::vector<SourceFile> files);

  private:
    struct Impl;

    explicit TranslationUnit(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;

    friend class Intellisense;
};

class Intellisense final {
  public:
    Intellisense();
    Intellisense(Intellisense&&) noexcept;
    Intellisense& operator=(Intellisense&&) noexcept;
    Intellisense(const Intellisense&) = delete;
    Intellisense& operator=(const Intellisense&) = delete;
    ~Intellisense();

    [[nodiscard]] TranslationUnit parse(std::string root_path, std::vector<SourceFile> files,
                                        const CompilerOptions& options = {}) const;

  private:
    struct Impl;
    std::shared_ptr<Impl> implementation_;

    friend class TranslationUnit;
};

} // namespace hlsl_intellisense::dxc
