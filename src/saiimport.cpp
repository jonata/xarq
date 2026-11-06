// PaintTool SAI (.sai, version 1), after the file notes in Wunkolo's libsai (MIT), which this follows for the
// page key below. The file is 4 KB pages, each encrypted: every 512th page is a table holding the next
// pages' checksums (the data pages' keys) and chain links. Over the pages sits a small file system, folders
// of 64-byte entries from page 2. Its files: "canvas" (size), "thumbnail" (BGRA), "laytbl" (layer ids, bottom
// first) and "layers/<id>": a header (type, bounds, opacity in percent, visibility, clipping, blend tag), a
// tag stream (name, parent folder), then for a raster layer a map of its 32×32 tiles and, for each painted
// one, eight PackBits streams of which the first four are B, G, R and A, premultiplied. A folder is listed
// before its members, a clipping layer after the layer it clips to.
#include "prepress.h"
#include <QHash>
#include <QImage>
#include <QSet>
#include <QStringDecoder>
#include <cstring>
#include <functional>

namespace xara {
namespace {

const qint64 kMaxPixels = qint64(1) << 28, kMaxTotalPixels = qint64(1) << 29;

const quint32 kKey[256] = {
    0x9913D29E, 0x83F58D3D, 0xD0BE1526, 0x86442EB7, 0x7EC69BFB, 0x89D75F64, 0xFB51B239, 0xFF097C56,
    0xA206EF1E, 0x973D668D, 0xC383770D, 0x1CB4CCEB, 0x36F7108B, 0x40336BCD, 0x84D123BD, 0xAFEF5DF3,
    0x90326747, 0xCBFFA8DD, 0x25B94703, 0xD7C5A4BA, 0xE40A17A0, 0xEADAE6F2, 0x6B738250, 0x76ECF24A,
    0x6F2746CC, 0x9BF95E24, 0x1ECA68C5, 0xE71C5929, 0x7817E56C, 0x2F99C471, 0x395A32B9, 0x61438343,
    0x5E3E4F88, 0x80A9332C, 0x1879C69F, 0x7A03D354, 0x12E89720, 0xF980448E, 0x03643576, 0x963C1D7B,
    0xBBED01D6, 0xC512A6B1, 0x51CB492B, 0x44BADEC9, 0xB2D54BC1, 0x4E7C2893, 0x1531C9A3, 0x43A32CA5,
    0x55B25A87, 0x70D9FA79, 0xEF5B4AE3, 0x8AE7F495, 0x923A8505, 0x1D92650C, 0xC94A9A5C, 0x27D4BB14,
    0x1372A9F7, 0x0C19A7FE, 0x64FA1A53, 0xF1A2EB6D, 0x9FEB910F, 0x4CE10C4E, 0x20825601, 0x7DFC98C4,
    0xA046C808, 0x8E90E7BE, 0x601DE357, 0xF360F37C, 0x00CD6F77, 0xCC6AB9D4, 0x24CC4E78, 0xAB1E0BFC,
    0x6A8BC585, 0xFD70ABF0, 0xD4A75261, 0x1ABF5834, 0x45DCFE17, 0x5F67E136, 0x948FD915, 0x65AD9EF5,
    0x81AB20E9, 0xD36EAF42, 0x0F7F45C7, 0x1BAE72D9, 0xBE116AC6, 0xDF58B4D5, 0x3F0B960E, 0xC2613F98,
    0xB065F8B0, 0x6259F975, 0xC49AEE84, 0x29718963, 0x0B6D991D, 0x09CF7A37, 0x692A6DF8, 0x67B68B02,
    0x2E10DBC2, 0x6C34E93C, 0xA84B50A1, 0xAC6FC0BB, 0x5CA6184C, 0x34E46183, 0x42B379A9, 0x79883AB6,
    0x08750921, 0x35AF2B19, 0xF7AA886A, 0x49F281D3, 0xA1768059, 0x14568CFD, 0x8B3625F6, 0x3E1B2D9D,
    0xF60E14CE, 0x1157270A, 0xDB5C7EB3, 0x738A0AFA, 0x19C248E5, 0x590CBD62, 0x7B37C312, 0xFC00B148,
    0xD808CF07, 0xD6BD1C82, 0xBD50F1D8, 0x91DEA3B8, 0xFA86B340, 0xF5DF2A80, 0x9A7BEA6E, 0x1720B8F1,
    0xED94A56B, 0xBF02BE28, 0x0D419FA8, 0x073B4DBC, 0x829E3144, 0x029F43E1, 0x71E6D51F, 0xA9381F09,
    0x583075E0, 0xE398D789, 0xF0E31106, 0x75073EB5, 0x5704863E, 0x6EF1043B, 0xBC407F33, 0x8DBCFB25,
    0x886C8F22, 0x5AF4DD7A, 0x2CEACA35, 0x8FC969DC, 0x9DB8D6B4, 0xC65EDC2F, 0xE60F9316, 0x0A84519A,
    0x3A294011, 0xDCF3063F, 0x41621623, 0x228CB75B, 0x28E9D166, 0xAE631B7F, 0x06D8C267, 0xDA693C94,
    0x54A5E860, 0x7C2170F4, 0xF2E294CB, 0x5B77A0F9, 0xB91522A6, 0xEC549500, 0x10DD78A7, 0x3823E458,
    0x77D3635A, 0x018E3069, 0xE039D055, 0xD5C341BF, 0x9C2400EA, 0x85C0A1D1, 0x66059C86, 0x0416FF1A,
    0xE27E05C8, 0xB19C4C2D, 0xFE4DF58F, 0xD2F0CE2A, 0x32E013C0, 0xEED637D7, 0xE9FEC1E8, 0xA4890DCA,
    0xF4180313, 0x7291738C, 0xE1B053A2, 0x9801267E, 0x2DA15BDB, 0xADC4DA4F, 0xCF95D474, 0xC0265781,
    0x1F226CED, 0xA7472952, 0x3C5F0273, 0xC152BA68, 0xDD66F09B, 0x93C7EDCF, 0x4F147404, 0x3193425D,
    0x26B5768A, 0x0E683B2E, 0x952FDF30, 0x2A6BAE46, 0xA3559270, 0xB781D897, 0xEB4ECB51, 0xDE49394D,
    0x483F629C, 0x2153845E, 0xB40D64E2, 0x47DB0ED0, 0x302D8E4B, 0x4BF8125F, 0x2BD2B0AC, 0x3DC836EC,
    0xC7871965, 0xB64C5CDE, 0x9EA8BC27, 0xD1853490, 0x3B42EC6F, 0x63A4FD91, 0xAA289D18, 0x4D2B1E49,
    0xB8A060AD, 0xB5F6C799, 0x6D1F7D1C, 0xBA8DAAE6, 0xE51A0FC3, 0xD94890E7, 0x167DF6D2, 0x879BCD41,
    0x5096AC1B, 0x05ACB5DA, 0x375D24EE, 0x7F2EB6AA, 0xA535F738, 0xCAD0AD10, 0xF8456E3A, 0x23FD5492,
    0xB3745532, 0x53C1A272, 0x469DFCDF, 0xE897BF7D, 0xA6BBE2AE, 0x68CE38AF, 0x5D783D0B, 0x524F21E4,
    0x4A257B31, 0xCE7A07B2, 0x562CE045, 0x33B708A4, 0x8CEE8AEF, 0xC8FB71FF, 0x74E52FAB, 0xCDB18796,};

quint32 keySum(quint32 v){ return kKey[v >> 24] + kKey[(v >> 16) & 255] + kKey[(v >> 8) & 255] + kKey[v & 255]; }
quint32 le32(const uchar* p){ return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24; }

// The decrypted pages, and the files they chain into.
struct Vfs {
    const uchar* p; qint64 pages;
    QHash<quint32, QVector<quint32>> tables;
    int badPages = 0;

    const QVector<quint32>& table(quint32 index){
        auto it = tables.find(index);
        if(it != tables.end()) return it.value();
        QVector<quint32> w(1024, 0);
        if(index < pages){
            quint32 prev = index & ~0x1ffu;
            for(int k = 0; k < 1024; ++k){
                const quint32 cur = le32(p + qint64(index) * 4096 + k * 4);
                const quint32 x = (prev ^ cur) ^ keySum(prev);
                prev = cur; w[k] = x << 16 | x >> 16;
            }
        }
        return tables.insert(index, w).value();
    }
    quint32 next(quint32 index){ return table(index & ~0x1ffu)[(index & 0x1ff) * 2 + 1]; }
    // One data page into `out`; false if it isn't there or its checksum doesn't hold.
    bool page(quint32 index, quint32* out){
        if(index >= pages || (index & 0x1ff) == 0) return false;
        const quint32 check = table(index & ~0x1ffu)[(index & 0x1ff) * 2];
        quint32 prev = check, sum = 0;
        for(int k = 0; k < 1024; ++k){
            const quint32 cur = le32(p + qint64(index) * 4096 + k * 4);
            out[k] = cur - (prev ^ keySum(prev));
            prev = cur;
            sum = (sum << 1 | sum >> 31) ^ out[k];
        }
        if((sum | 1) != check){ ++badPages; return false; }
        return true;
    }
    QByteArray read(quint32 first, quint32 size){
        QByteArray out;
        if(size > (1u << 30) || qint64(size) > pages * 4096) return out;
        out.reserve(int(size));
        QSet<quint32> seen;
        quint32 buf[1024];
        for(quint32 at = first; at && out.size() < qint64(size); at = next(at)){
            if(seen.contains(at) || !page(at, buf)) return QByteArray();
            seen.insert(at);
            out.append(reinterpret_cast<const char*>(buf), int(qMin<qint64>(4096, qint64(size) - out.size())));
        }
        return out.size() == qint64(size) ? out : QByteArray();
    }
    // Every file under the folder at page `first`, by path.
    void walk(quint32 first, const QString& path, QHash<QString, QPair<quint32, quint32>>& files, int depth, QSet<quint32>& seen){
        quint32 buf[1024];
        for(quint32 at = first; at && depth < 8; at = next(at)){
            if(seen.contains(at) || seen.size() > 100000 || !page(at, buf)) return;
            seen.insert(at);
            const uchar* e = reinterpret_cast<const uchar*>(buf);
            for(int k = 0; k < 64; ++k, e += 64){
                if(!le32(e)) return;
                const QString name = path + QString::fromLatin1(reinterpret_cast<const char*>(e + 4), int(qstrnlen(reinterpret_cast<const char*>(e + 4), 32)));
                if(e[38] == 0x10) walk(le32(e + 40), name + QLatin1Char('/'), files, depth + 1, seen);
                else if(files.size() < 100000) files.insert(name, qMakePair(le32(e + 40), le32(e + 44)));
            }
        }
    }
};

// PackBits into `count` bytes at every 4th byte of `d`.
bool packBits(const uchar* s, qint64 n, uchar* d, int count){
    qint64 i = 0; int o = 0;
    while(o < count){
        if(i >= n) return false;
        const int len = s[i++];
        if(len == 128) continue;
        if(len < 128){
            const int run = len + 1;
            if(run > count - o || run > n - i) return false;
            for(int k = 0; k < run; ++k) d[(o++) * 4] = s[i++];
        } else {
            const int run = 257 - len;
            if(run > count - o || i >= n) return false;
            const uchar v = s[i++];
            for(int k = 0; k < run; ++k) d[(o++) * 4] = v;
        }
    }
    return true;
}

QString layerName(const QByteArray& raw){
    const QByteArray b = raw.left(int(qstrnlen(raw.constData(), size_t(raw.size()))));
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString s = utf8.decode(b);
    if(!utf8.hasError()) return s;
    QStringDecoder sjis("Shift_JIS");                  // SAI is a Japanese program of the ANSI era
    if(sjis.isValid()){ const QString t = sjis.decode(b); if(!sjis.hasError()) return t; }
    return QString::fromLatin1(b);
}

// SAI's blend tags (stored as little-endian words of the big-endian tag) as the PSD route's keys.
QByteArray blendKey(quint32 tag, bool* known){
    const char c[4] = { char(tag >> 24), char(tag >> 16), char(tag >> 8), char(tag) };
    const QByteArray t(c, 4);
    *known = true;
    if(t == "norm" || t == "pass") return "norm";
    if(t == "mul ") return "mul ";
    if(t == "scrn") return "scrn";
    if(t == "over") return "over";
    if(t == "add ") return "lddg";      // "Luminosity": adds
    if(t == "sub ") return "lbrn";      // "Shade": linear burn
    if(t == "adsb") return "lLit";      // "Lumi & Shade": adds above the middle, burns below
    *known = false;                     // "Binary Colour" and anything newer
    return "norm";
}

struct SLayer {
    quint32 id = 0, parent = 0, type = 0, blend = 0; int x = 0, y = 0, w = 0, h = 0, opacity = 100;
    bool visible = true, clipping = false; QString name; QByteArray data; qint64 pixelsAt = 0;
    QVector<int> kids;
};

} // namespace

// For the self-test: plain pages encrypted as SAI writes them. Page 0 is the table, its chain links filled in
// by the caller; the data pages' checksums are put in it here. One table's worth (512 pages) at most.
QByteArray saiEncryptPages(QVector<QByteArray> pages){
    QByteArray out;
    if(pages.isEmpty() || pages.size() > 512) return out;
    for(QByteArray& pg : pages) pg = pg.leftJustified(4096, '\0', true);
    auto word = [](const QByteArray& b, int k){ return le32(reinterpret_cast<const uchar*>(b.constData()) + k * 4); };
    auto put = [](QByteArray& b, int k, quint32 v){ for(int i = 0; i < 4; ++i) b[k * 4 + i] = char(v >> (8 * i)); };
    for(int i = 1; i < pages.size(); ++i){
        quint32 sum = 0;
        for(int k = 0; k < 1024; ++k) sum = (sum << 1 | sum >> 31) ^ word(pages[i], k);
        put(pages[0], i * 2, sum | 1);
    }
    for(int i = 0; i < pages.size(); ++i){
        QByteArray enc(4096, '\0');
        quint32 prev = i == 0 ? 0 : word(pages[0], i * 2);
        for(int k = 0; k < 1024; ++k){
            const quint32 w = word(pages[i], k);
            const quint32 cur = i == 0 ? ((w << 16 | w >> 16) ^ keySum(prev) ^ prev) : w + (prev ^ keySum(prev));
            put(enc, k, cur); prev = cur;
        }
        out += enc;
    }
    return out;
}

Document importSaiDocument(const QByteArray& f, SaiImportStats* st){
    Document doc;
    SaiImportStats local; if(!st) st = &local;
    if(f.size() < 3 * 4096) return doc;
    Vfs v{ reinterpret_cast<const uchar*>(f.constData()), f.size() / 4096, {} };
    QHash<QString, QPair<quint32, quint32>> files;
    QSet<quint32> seen;
    v.walk(2, QString(), files, 0, seen);
    auto file = [&](const QString& name){ const auto it = files.constFind(name); return it == files.constEnd() ? QByteArray() : v.read(it->first, it->second); };

    const QByteArray canvas = file(QStringLiteral("canvas"));
    if(canvas.size() < 12) return doc;
    const qint64 W = le32(reinterpret_cast<const uchar*>(canvas.constData()) + 4), H = le32(reinterpret_cast<const uchar*>(canvas.constData()) + 8);
    if(W < 1 || H < 1 || W > 1000000 || H > 1000000 || W * H > kMaxPixels) return doc;
    doc.pageSize = QSizeF(W, H);

    // The layers in stack order, bottom first, and which folder each belongs to.
    QVector<SLayer> layers; QHash<quint32, int> byId;
    const QByteArray table = file(QStringLiteral("laytbl"));
    const uchar* t = reinterpret_cast<const uchar*>(table.constData());
    const qint64 count = table.size() >= 4 ? qMin<qint64>(le32(t), (table.size() - 4) / 8) : 0;
    for(qint64 k = 0; k < count && k < 10000; ++k){
        const quint32 id = le32(t + 4 + k * 8);
        const QByteArray d = file(QStringLiteral("layers/%1").arg(id, 8, 16, QLatin1Char('0')));
        if(d.size() < 37){ ++st->missing; continue; }
        const uchar* h = reinterpret_cast<const uchar*>(d.constData());
        SLayer L; L.id = id; L.type = le32(h);
        L.x = int(qBound(-10000000, qint32(le32(h + 8)), 10000000)); L.y = int(qBound(-10000000, qint32(le32(h + 12)), 10000000));
        L.w = int(qMin<quint32>(le32(h + 16), 1000000)); L.h = int(qMin<quint32>(le32(h + 20), 1000000));
        L.opacity = qMin(int(h[28]), 100); L.visible = h[29] != 0; L.clipping = h[31] != 0; L.blend = le32(h + 33);
        qint64 o = 37;
        while(o + 8 <= d.size()){                                            // the tag stream, ended by a zero tag
            const quint32 tag = le32(h + o), size = le32(h + o + 4);
            if(!tag){ o += 4; break; }
            o += 8;
            if(size > quint64(d.size() - o)){ o = d.size(); break; }
            if(tag == 0x6e616d65u) L.name = layerName(d.mid(int(o), int(size)));                 // "name"
            else if(tag == 0x70666964u && size >= 4) L.parent = le32(h + o);                     // "pfid"
            o += size;
        }
        L.pixelsAt = o; L.data = d;
        if(!byId.contains(id)) byId.insert(id, layers.size());
        layers << L;
    }
    QVector<int> top;
    for(int k = 0; k < layers.size(); ++k){
        const int at = layers[k].parent ? byId.value(layers[k].parent, -1) : -1;
        if(at >= 0 && at != k && layers[at].type == 8) layers[at].kids << k; else top << k;
    }

    qint64 budget = kMaxTotalPixels;
    // A raster layer's pixels, straight alpha, over its tiles' bounds; false if none.
    auto pixels = [&](const SLayer& L, QImage& img, QPoint& at) -> bool {
        const int tx = L.w / 32, ty = L.h / 32;
        if(tx < 1 || ty < 1 || qint64(tx) * ty * 1024 > kMaxPixels || qint64(tx) * ty * 1024 > budget) return false;
        const uchar* s = reinterpret_cast<const uchar*>(L.data.constData());
        const qint64 n = L.data.size();
        qint64 o = L.pixelsAt;
        if(o + qint64(tx) * ty > n){ ++st->badTiles; return false; }
        const uchar* map = s + o; o += qint64(tx) * ty;
        img = QImage(tx * 32, ty * 32, QImage::Format_ARGB32);
        if(img.isNull()) return false;
        budget -= qint64(tx) * ty * 1024;
        img.fill(Qt::transparent);
        uchar tile[4096];
        bool ok = true;
        for(int j = 0; j < ty && ok; ++j) for(int i = 0; i < tx && ok; ++i){
            if(!map[j * tx + i]) continue;
            std::memset(tile, 0, sizeof tile);
            for(int c = 0; c < 8; ++c){                                    // B, G, R, A, then four more not needed
                if(o + 2 > n){ ok = false; break; }
                const int size = s[o] | s[o + 1] << 8; o += 2;
                if(size > n - o){ ok = false; break; }
                if(c < 4 && !packBits(s + o, size, tile + c, 1024)){ ++st->badTiles; std::memset(tile, 0, sizeof tile); }
                o += size;
            }
            if(!ok){ ++st->badTiles; break; }
            for(int y = 0; y < 32; ++y){
                QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(j * 32 + y)) + i * 32;
                const uchar* q = tile + y * 128;
                for(int x = 0; x < 32; ++x, q += 4){
                    const int a = q[3];
                    if(!a){ d[x] = 0; continue; }
                    auto un = [a](int c){ return qMin(255, (c * 255 + a / 2) / a); };
                    d[x] = qRgba(un(q[2]), un(q[1]), un(q[0]), a);
                }
            }
        }
        at = QPoint(L.x, L.y);
        return true;
    };
    // Only what's painted becomes the object; false if nothing is.
    auto crop = [&](QImage& img, QPoint& at) -> bool {
        int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
        for(int y = 0; y < img.height(); ++y){
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for(int x = 0; x < img.width(); ++x) if(qAlpha(row[x])){ x0 = qMin(x0, x); x1 = qMax(x1, x); y0 = qMin(y0, y); y1 = y; }
        }
        if(x1 < 0){ ++st->empty; return false; }
        const QRect box(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        if(box.size() != img.size()) img = img.copy(box);
        at += box.topLeft();
        return true;
    };
    // Clipping: a layer only shows where the layer it is clipped to has paint.
    auto clip = [](QImage& img, QPoint at, const QImage& base, QPoint bat){
        for(int y = 0; y < img.height(); ++y){
            QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
            const int sy = at.y() + y - bat.y();
            const QRgb* b = sy >= 0 && sy < base.height() ? reinterpret_cast<const QRgb*>(base.constScanLine(sy)) : nullptr;
            for(int x = 0; x < img.width(); ++x){
                const int sx = at.x() + x - bat.x();
                const int a = b && sx >= 0 && sx < base.width() ? qAlpha(b[sx]) : 0;
                row[x] = (row[x] & 0xffffff) | uint(qAlpha(row[x]) * a / 255) << 24;
            }
        }
    };

    std::function<QVector<Shape>(const QVector<int>&, int)> build = [&](const QVector<int>& list, int depth) -> QVector<Shape> {
        QVector<Shape> out;
        QImage base; QPoint bat; bool haveBase = false;
        for(int k : list){
            const SLayer& L = layers[k];
            bool known = true; const QByteArray key = blendKey(L.blend, &known);
            if(!known) ++st->unknownModes;
            Shape s;
            if(L.type == 8){                                                  // a folder ("set")
                if(depth >= 64) continue;
                s.type = ShapeType::Group; s.name = L.name.isEmpty() ? QStringLiteral("Set") : L.name;
                s.children = build(L.kids, depth + 1);
                if(s.children.isEmpty()) continue;
                s.isolate = L.blend != 0x70617373u || L.opacity < 100;        // "pass" composites with what's below
                ++st->groups;
                haveBase = false;
            } else if(L.type == 3){
                QImage img; QPoint at;
                const bool painted = pixels(L, img, at);
                if(painted && L.clipping && haveBase){ clip(img, at, base, bat); ++st->clipped; }
                if(!painted || !crop(img, at)){
                    if(!L.clipping){ base = QImage(); haveBase = true; }   // what clips to an empty layer shows nothing
                    continue;
                }
                if(!L.clipping){ base = img; bat = at; haveBase = true; }
                s = psdBitmapShape(img, at.x(), at.y(), L.name.isEmpty() ? QStringLiteral("Layer") : L.name);
                ++st->layers;
            } else {                                                          // linework and the like
                ++st->otherLayers;
                continue;
            }
            s.opacity = L.opacity / 100.0;
            // SAI adds the faded layer, min(1, under + layer × opacity), where a blend with opacity mixes the
            // full sum back with what's under; they differ where the sum saturates. Folding the opacity into
            // the pixels gives SAI's result.
            if(key == "lddg" && L.type == 3 && L.opacity < 100){
                QImage& img = s.fill.image;
                for(int y = 0; y < img.height(); ++y){
                    QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
                    for(int x = 0; x < img.width(); ++x) row[x] = (row[x] & 0xffffff) | uint((qAlpha(row[x]) * L.opacity + 50) / 100) << 24;
                }
                s.fill.origImage = img;
                s.opacity = 1.0;
            }
            s.visible = L.visible;
            psdApplyBlend(s, key);
            if(!L.visible) ++st->hidden;
            out << s;
        }
        return out;
    };
    doc.shapes = build(top, 0);
    st->badTiles += v.badPages;

    // With nothing readable where something was meant to be, the thumbnail stands in.
    if(doc.shapes.isEmpty() && (st->badTiles || st->missing || st->otherLayers)){
        const QByteArray th = file(QStringLiteral("thumbnail"));
        const uchar* q = reinterpret_cast<const uchar*>(th.constData());
        const qint64 tw = th.size() >= 12 ? le32(q) : 0, thh = th.size() >= 12 ? le32(q + 4) : 0;
        if(tw > 0 && thh > 0 && tw * thh <= kMaxPixels && th.size() >= 12 + tw * thh * 4){
            QImage img(int(tw), int(thh), QImage::Format_ARGB32);
            for(int y = 0; y < thh; ++y){
                const uchar* r = q + 12 + qint64(y) * tw * 4;
                QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
                for(int x = 0; x < tw; ++x, r += 4) d[x] = qRgba(r[2], r[1], r[0], r[3]);
            }
            Shape s = psdBitmapShape(img, 0, 0, QStringLiteral("Preview"));
            s.transform = QTransform::fromScale(double(W) / tw, double(H) / thh);
            doc.shapes = { s };
            st->thumbnailOnly = true;
        }
    }
    return doc;
}

} // namespace xara
