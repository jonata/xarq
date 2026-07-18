// otshape.cpp — see otshape.h. HarfBuzz shaping bridged to Qt's QRawFont for outlines.
#include "otshape.h"
#include <QRawFont>
#include <QByteArray>
#include <hb.h>
#include <cstdlib>
#include <cstring>

namespace xara {

// HarfBuzz asks for each SFNT table by tag; hand it QRawFont's copy of that table so HB shapes the
// exact same face Qt will draw. The blob owns a heap copy so it stays valid for the face's lifetime.
static hb_blob_t* tableCallback(hb_face_t*, hb_tag_t tag, void* user){
    QRawFont* rf = static_cast<QRawFont*>(user);
    const char t[4] = { char((tag>>24)&0xff), char((tag>>16)&0xff), char((tag>>8)&0xff), char(tag&0xff) };
    const QByteArray data = rf->fontTable(QByteArray(t, 4));
    if(data.isEmpty()) return hb_blob_get_empty();
    char* buf = static_cast<char*>(std::malloc(size_t(data.size())));
    if(!buf) return hb_blob_get_empty();
    std::memcpy(buf, data.constData(), size_t(data.size()));
    return hb_blob_create(buf, unsigned(data.size()), HB_MEMORY_MODE_WRITABLE, buf, std::free);
}

bool otAvailable(){ return true; }   // HarfBuzz is linked; QRawFont validity is checked per-call

QPainterPath otShapeLine(const QString& text, const QFont& font, const QStringList& features,
                         double tracking, double* advanceOut, int* glyphCountOut, bool* hadMissingOut){
    QPainterPath out;
    if(advanceOut) *advanceOut=0; if(glyphCountOut) *glyphCountOut=0; if(hadMissingOut) *hadMissingOut=false;
    if(text.isEmpty()) return out;

    QRawFont rf = QRawFont::fromFont(font);
    if(!rf.isValid()) return out;

    hb_face_t* face = hb_face_create_for_tables(tableCallback, &rf, nullptr);
    if(!face) return out;
    unsigned upem = hb_face_get_upem(face); if(!upem) upem = 1000;
    hb_font_t* hbf = hb_font_create(face);
    hb_font_set_scale(hbf, int(upem), int(upem));   // positions come out in font units → we scale to px

    hb_buffer_t* buf = hb_buffer_create();
    const QByteArray u8 = text.toUtf8();
    hb_buffer_add_utf8(buf, u8.constData(), u8.size(), 0, u8.size());
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_guess_segment_properties(buf);        // fills in language (keeps our LTR/Latin)

    QVector<hb_feature_t> feats;
    for(const QString& fs : features){ hb_feature_t f;
        if(hb_feature_from_string(fs.toUtf8().constData(), -1, &f)) feats.push_back(f); }

    hb_shape(hbf, buf, feats.constData(), feats.size());

    unsigned n=0;
    const hb_glyph_info_t*     gi = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* gp = hb_buffer_get_glyph_positions(buf, &n);
    const double px = rf.pixelSize() > 0 ? rf.pixelSize() : font.pixelSize();
    const double sc = upem>0 ? px/double(upem) : 1.0;   // font units → pixels
    double penX=0;
    for(unsigned i=0;i<n;++i){
        const quint32 gid = gi[i].codepoint;            // after shaping, codepoint == glyph index
        if(gid==0 && hadMissingOut) *hadMissingOut=true;   // .notdef — the font lacks this character
        QPainterPath g = rf.pathForGlyph(gid);          // outline in px, baseline at origin, y-down (Qt)
        const double xo = gp[i].x_offset*sc, yo = gp[i].y_offset*sc;
        if(!g.isEmpty()) out.addPath(g.translated(penX+xo, -yo));   // +y_offset (up) = negative Qt y
        penX += gp[i].x_advance*sc + tracking;          // QRawFont drops QFont letter-spacing → add it back
    }
    if(advanceOut) *advanceOut=penX;
    if(glyphCountOut) *glyphCountOut=int(n);

    hb_buffer_destroy(buf);
    hb_font_destroy(hbf);
    hb_face_destroy(face);
    return out;
}

} // namespace xara
