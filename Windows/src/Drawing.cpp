#include "Drawing.h"

#include "Settings.h"

using namespace Gdiplus;

namespace filedrop::win {

namespace {
bool fontAvailable(const wchar_t* family) {
    FontFamily f(family);
    return f.GetLastStatus() == Ok && f.IsAvailable();
}
}  // namespace

Theme Theme::current() {
    Theme t;
    t.dark = appsUseDarkTheme();
    if (t.dark) {
        t.background = Color(255, 44, 44, 46);
        t.text = Color(235, 255, 255, 255);
        t.secondaryText = Color(150, 255, 255, 255);
        t.tile = Color(14, 255, 255, 255);
        t.tileHover = Color(26, 255, 255, 255);
        t.button = Color(20, 255, 255, 255);
        t.dashed = Color(110, 255, 255, 255);
        t.divider = Color(28, 255, 255, 255);
    } else {
        t.background = Color(255, 246, 246, 246);
        t.text = Color(230, 0, 0, 0);
        t.secondaryText = Color(140, 0, 0, 0);
        t.tile = Color(10, 0, 0, 0);
        t.tileHover = Color(20, 0, 0, 0);
        t.button = Color(16, 0, 0, 0);
        t.dashed = Color(110, 0, 0, 0);
        t.divider = Color(26, 0, 0, 0);
    }
    t.accent = Color(255, 10, 132, 255);
    t.accentSoft = Color(38, 10, 132, 255);
    t.success = Color(255, 52, 199, 89);
    t.warning = Color(255, 255, 149, 0);
    return t;
}

const wchar_t* iconFontFamily() {
    static const wchar_t* family = fontAvailable(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    return family;
}

const wchar_t* textFontFamily() {
    static const wchar_t* family = fontAvailable(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    return family;
}

void addRoundRect(GraphicsPath& path, const RectF& r, float radius) {
    float d = std::min(radius * 2, std::min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

HICON createTrayIcon(int size, bool darkTaskbar, double progress) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0));

    const Color fg = darkTaskbar ? Color(255, 255, 255, 255) : Color(255, 20, 20, 20);
    if (progress < 0) {
        FontFamily family(iconFontFamily());
        Font font(&family, size * 0.8f, FontStyleRegular, UnitPixel);
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
        SolidBrush brush(fg);
        g.DrawString(glyph::kApp, -1, &font, RectF(0, 0, static_cast<REAL>(size), static_cast<REAL>(size)), &fmt, &brush);
    } else {
        float pw = std::max(2.0f, size / 8.0f);
        RectF r(pw, pw, size - 2 * pw, size - 2 * pw);
        Pen track(Color(90, fg.GetR(), fg.GetG(), fg.GetB()), pw);
        g.DrawEllipse(&track, r);
        Pen arc(fg, pw);
        arc.SetStartCap(LineCapRound);
        arc.SetEndCap(LineCapRound);
        g.DrawArc(&arc, r, -90.0f, static_cast<REAL>(360.0 * std::clamp(progress, 0.02, 1.0)));
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

}  // namespace filedrop::win
