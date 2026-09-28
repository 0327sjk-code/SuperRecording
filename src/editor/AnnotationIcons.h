#pragma once
// Crisp code-native annotation icons, sharing the editor's buffered button rendering.
#include "editor/EditorChrome.h"
namespace qrec {
void DrawAnnotationIcon(HDC dc, RECT bounds, EditorButtonRole role, COLORREF color);
}
