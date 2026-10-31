// icc.h — real ICC colour management (Batch R7). Loads liblcms2.so.2 at RUNTIME via dlopen (no dev
// headers / link dependency needed), so the app degrades gracefully to the naive prepress transforms
// when Little-CMS is absent. Provides an accurate sRGB↔CMYK soft-proof and CMYK separation through
// actual printer profiles (system FOGRA/SWOP/GRACoL .icc files).
#pragma once
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace xara {

// A document's embedded profile to sRGB, with Photoshop's default rendering: relative colorimetric with black
// point compensation. ok() is false without lcms2, for a profile it can't read, or one of another colour space.
class IccToSrgb {
public:
    IccToSrgb(const QByteArray& profile, int components);    // 1 grey, 3 RGB, 4 CMYK (ink, 255 = full)
    ~IccToSrgb();
    IccToSrgb(const IccToSrgb&) = delete; IccToSrgb& operator=(const IccToSrgb&) = delete;
    bool ok() const { return m_xf != nullptr; }
    void convert(const uchar* in, uchar* rgb, int pixels) const;
private:
    void* m_xf = nullptr;
};

class Icc {
public:
    static Icc& instance();

    bool available() const;                       // liblcms2 loaded AND an sRGB + CMYK profile are ready
    QStringList cmykProfiles() const;             // system CMYK .icc profiles (path list)
    QString     currentCmykProfile() const;       // the profile used for proof/separation ("" = none)
    bool        setCmykProfile(const QString& path);   // choose a CMYK profile; rebuilds transforms
    void        setRenderingIntent(int intent, bool blackPointCompensation);  // 0 perc,1 rel-col,2 sat,3 abs-col; rebuilds
    int         renderingIntent() const;
    bool        blackPointCompensation() const;

    // sRGB → (CMYK profile) → sRGB round-trip: how the image looks printed on that stock. Alpha preserved.
    // Returns a null QImage if ICC is unavailable (caller falls back to the naive proof).
    QImage softProof(const QImage& src) const;

    // sRGB → CMYK: fills `planes` with 4 Grayscale8 ink-coverage images (C, M, Y, K). Returns false if
    // ICC is unavailable. Used by the separations exporter for real plates.
    bool toCmykPlanes(const QImage& src, QVector<QImage>& planes) const;

private:
    Icc();
    ~Icc();
    Icc(const Icc&) = delete; Icc& operator=(const Icc&) = delete;
    struct Impl; Impl* d;
};

} // namespace xara
