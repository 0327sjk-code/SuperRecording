#pragma once
#include "common/Types.h"
#include <filesystem>
#include <string_view>
namespace annotation_tests {
void Expect(bool condition,std::wstring_view description);
int Failures();
void ModelTests();
qrec::ExportRequest MediaTests(const std::filesystem::path& root);
void UiTests(const std::filesystem::path& root,const qrec::ExportRequest& request);
void SelectionUiTests();
void HybridProbe(const std::filesystem::path& root);
}
