#include <dlfcn.h>
#include <dxcisense.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct Release {
    template <typename Interface> void operator()(Interface* value) const noexcept {
        if (value != nullptr) {
            value->Release();
        }
    }
};

template <typename Interface> using ComPtr = std::unique_ptr<Interface, Release>;

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error{std::string{operation} + " failed"};
    }
}

template <typename Interface, typename Create> ComPtr<Interface> obtain(Create create) {
    Interface* value{};
    check(create(&value), "DXC interface creation");
    return ComPtr<Interface>{value};
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: hlsl-dxc-skipped-ranges-repro <libdxcompiler.so>\n";
        return 2;
    }
    auto* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        std::cerr << dlerror() << '\n';
        return 1;
    }
    try {
        auto create = reinterpret_cast<DxcCreateInstanceProc>(dlsym(module, "DxcCreateInstance"));
        if (create == nullptr) {
            throw std::runtime_error{"DxcCreateInstance was not exported"};
        }
        {
            auto sense = obtain<IDxcIntelliSense>([&](auto** value) {
                return create(CLSID_DxcIntelliSense, __uuidof(IDxcIntelliSense),
                              reinterpret_cast<void**>(value));
            });
            auto index = obtain<IDxcIndex>([&](auto** value) { return sense->CreateIndex(value); });
            constexpr std::array names{"/tmp/main.hlsl", "/virtual/include.hlsli",
                                       "/tmp/include.hlsli"};
            const std::array<std::string, 3> texts{
                "#include \"/tmp/include.hlsli\"\n#if 0\nfloat skippedValue;\n#endif\n"
                "float4 main() : SV_Target { return includedValue.xxxx; }\n",
                "static const float includedValue = 1.0;\n",
                "static const float includedValue = 1.0;\n"};
            std::array<ComPtr<IDxcUnsavedFile>, 3> files;
            std::array<IDxcUnsavedFile*, 3> raw_files{};
            for (std::size_t i = 0; i < files.size(); ++i) {
                files[i] = obtain<IDxcUnsavedFile>([&](auto** value) {
                    return sense->CreateUnsavedFile(names[i], texts[i].c_str(),
                                                    static_cast<unsigned>(texts[i].size()), value);
                });
                raw_files[i] = files[i].get();
            }
            auto unit = obtain<IDxcTranslationUnit>([&](auto** value) {
                return index->ParseTranslationUnit(
                    names.front(), nullptr, 0, raw_files.data(),
                    static_cast<unsigned>(raw_files.size()),
                    static_cast<DxcTranslationUnitFlags>(
                        DxcTranslationUnitFlags_DetailedPreprocessingRecord |
                        DxcTranslationUnitFlags_UseCallerThread),
                    value);
            });
            for (std::size_t i = 0; i < names.size(); ++i) {
                auto file =
                    obtain<IDxcFile>([&](auto** value) { return unit->GetFile(names[i], value); });
                unsigned count{};
                IDxcSourceRange** ranges{};
                std::cout << "GetSkippedRanges " << names[i] << std::endl;
                check(unit->GetSkippedRanges(file.get(), &count, &ranges), "GetSkippedRanges");
                for (unsigned j = 0; j < count; ++j) {
                    ranges[j]->Release();
                }
                CoTaskMemFree(ranges);
                if (count != (i == 0 ? 1U : 0U)) {
                    throw std::runtime_error{"Unexpected skipped-range count"};
                }
                std::cout << "ranges=" << count << '\n';
            }
        }
        dlclose(module);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        dlclose(module);
        return 1;
    }
}
