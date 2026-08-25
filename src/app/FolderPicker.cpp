#include "app/FolderPicker.h"

#include <shobjidl.h>
#include <wrl/client.h>

namespace qrec {

std::optional<std::filesystem::path> PickSaveDirectory(
    const HWND owner,
    const std::filesystem::path& currentDirectory) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT result = ::CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        return std::nullopt;
    }

    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
    }
    dialog->SetTitle(L"选择录屏保存位置");
    dialog->SetOkButtonLabel(L"选择此文件夹");

    Microsoft::WRL::ComPtr<IShellItem> initialFolder;
    if (!currentDirectory.empty() && SUCCEEDED(::SHCreateItemFromParsingName(
            currentDirectory.c_str(), nullptr, IID_PPV_ARGS(&initialFolder)))) {
        dialog->SetFolder(initialFolder.Get());
    }

    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::nullopt;
    }
    if (FAILED(result)) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) {
        return std::nullopt;
    }

    PWSTR rawPath = nullptr;
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || rawPath == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path path(rawPath);
    ::CoTaskMemFree(rawPath);
    return path;
}

}  // namespace qrec
