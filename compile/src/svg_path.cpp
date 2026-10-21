#include "xt/compile/svg_path.h"

#include <charconv>
#include <cmath>

#include "xt/charconv.h"
#include "xt/node.h"

/// SVG path data (`d`) parser. See docs/design/12-compiler.md §12.2.1, docs/tasks/T-110.md.

namespace xt::compile {

namespace {

constexpr float kPiSvgPath = 3.14159265358979323846f;
constexpr float kDegToRadSvgPath = kPiSvgPath / 180.0f;

bool isSeparator(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ','; }

bool isCommand(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

bool isDigitSvgPath(char c) { return c >= '0' && c <= '9'; }

/// Scanner over a `d` attribute: separators (whitespace and commas) are equivalent everywhere.
struct Scanner {
    std::string_view s;
    size_t i = 0;

    void skipSeparators() {
        while (i < s.size() && isSeparator(s[i])) {
            ++i;
        }
    }

    bool atEnd() {
        skipSeparators();
        return i >= s.size();
    }

    /// Reads one number, accepting the forms SVG allows (leading sign, no leading digit, exponent).
    bool number(float& out) {
        skipSeparators();
        size_t begin = i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        size_t digitsBegin = i;
        while (i < s.size() && isDigitSvgPath(s[i])) {
            ++i;
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && isDigitSvgPath(s[i])) {
                ++i;
            }
        }
        if (i == digitsBegin || (i == digitsBegin + 1 && s[digitsBegin] == '.')) {
            i = begin;
            return false;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            size_t expBegin = i;
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                ++i;
            }
            if (i < s.size() && isDigitSvgPath(s[i])) {
                while (i < s.size() && isDigitSvgPath(s[i])) {
                    ++i;
                }
            } else {
                i = expBegin;  // Not an exponent after all (e.g. "1e" or "2emph").
            }
        }
        // std::from_chars over the exact span, so "10-20" and "1.5.5" split the way SVG expects.
        double value = 0;
        // std::from_chars rejects a leading '+', which SVG allows, so skip it.
        const char* first = s.data() + (s[begin] == '+' ? begin + 1 : begin);
        const char* last = s.data() + i;
        std::from_chars_result r = xt::fromChars(first, last, value);
        if (r.ec != std::errc() || r.ptr != last) {
            i = begin;
            return false;
        }
        out = static_cast<float>(value);
        return true;
    }

    /// Reads an arc flag: a single `0` or `1`, which may be written with no separator after it.
    bool flag(bool& out) {
        skipSeparators();
        if (i >= s.size() || (s[i] != '0' && s[i] != '1')) {
            return false;
        }
        out = s[i] == '1';
        ++i;
        return true;
    }

    bool point(Vec2& out) { return number(out.x) && number(out.y); }
};

Vec2 add(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }

/// Appends one <= 90 degree arc segment (F.6.6's cubic approximation) in the ellipse's own space.
void appendArcSegment(SvgPath& path, Vec2 center, float rx, float ry, float cosRot, float sinRot,
                      float theta0, float dTheta) {
    float t = std::tan(dTheta * 0.5f);  // Maisonobe: tan(dTheta / 2), not / 4
    float alpha = std::sin(dTheta) * (std::sqrt(4.0f + 3.0f * t * t) - 1.0f) / 3.0f;
    float cos0 = std::cos(theta0);
    float sin0 = std::sin(theta0);
    float cos1 = std::cos(theta0 + dTheta);
    float sin1 = std::sin(theta0 + dTheta);

    auto toUser = [&](float ex, float ey) {
        return Vec2{center.x + cosRot * ex - sinRot * ey, center.y + sinRot * ex + cosRot * ey};
    };
    Vec2 p0 = toUser(rx * cos0, ry * sin0);
    Vec2 p1 = toUser(rx * cos1, ry * sin1);
    Vec2 d0 = toUser(-rx * sin0, ry * cos0);
    Vec2 d1 = toUser(-rx * sin1, ry * cos1);
    // toUser() includes the center offset, so subtract it back out of the derivatives.
    d0 = Vec2{d0.x - center.x, d0.y - center.y};
    d1 = Vec2{d1.x - center.x, d1.y - center.y};

    Vec2 c1 = Vec2{p0.x + alpha * d0.x, p0.y + alpha * d0.y};
    Vec2 c2 = Vec2{p1.x - alpha * d1.x, p1.y - alpha * d1.y};
    path.cubicTo(c1, c2, p1);
}

}  // namespace

void SvgPath::moveTo(Vec2 p) {
    verbs.push_back(static_cast<uint8_t>(PathVerb::Move));
    points.push_back(p.x);
    points.push_back(p.y);
}

void SvgPath::lineTo(Vec2 p) {
    verbs.push_back(static_cast<uint8_t>(PathVerb::Line));
    points.push_back(p.x);
    points.push_back(p.y);
}

void SvgPath::cubicTo(Vec2 c1, Vec2 c2, Vec2 p) {
    verbs.push_back(static_cast<uint8_t>(PathVerb::Cubic));
    points.push_back(c1.x);
    points.push_back(c1.y);
    points.push_back(c2.x);
    points.push_back(c2.y);
    points.push_back(p.x);
    points.push_back(p.y);
}

void SvgPath::close() { verbs.push_back(static_cast<uint8_t>(PathVerb::Close)); }

void appendSvgArc(SvgPath& path, Vec2 start, float rx, float ry, float xAxisRotationDeg, bool largeArc,
                  bool sweep, Vec2 end) {
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx <= 0.0f || ry <= 0.0f || (start.x == end.x && start.y == end.y)) {
        // F.6.2: a zero radius (or a zero-length arc) degenerates to a straight line.
        path.lineTo(end);
        return;
    }

    float rot = xAxisRotationDeg * kDegToRadSvgPath;
    float cosRot = std::cos(rot);
    float sinRot = std::sin(rot);

    // F.6.5 step 1: the midpoint in the rotated frame.
    float dx2 = (start.x - end.x) * 0.5f;
    float dy2 = (start.y - end.y) * 0.5f;
    float x1p = cosRot * dx2 + sinRot * dy2;
    float y1p = -sinRot * dx2 + cosRot * dy2;

    // F.6.6: scale the radii up when they are too small to span the chord.
    float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f) {
        float scale = std::sqrt(lambda);
        rx *= scale;
        ry *= scale;
    }

    // F.6.5 step 2: the center in the rotated frame.
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float factor = den > 0.0f ? std::sqrt(num > 0.0f ? num / den : 0.0f) : 0.0f;
    if (largeArc == sweep) {
        factor = -factor;
    }
    float cxp = factor * rx * y1p / ry;
    float cyp = -factor * ry * x1p / rx;

    // F.6.5 step 3: back to user space.
    Vec2 center{cosRot * cxp - sinRot * cyp + (start.x + end.x) * 0.5f,
                sinRot * cxp + cosRot * cyp + (start.y + end.y) * 0.5f};

    // F.6.5 step 4: the start angle and the swept angle.
    float ux = (x1p - cxp) / rx;
    float uy = (y1p - cyp) / ry;
    float vx = (-x1p - cxp) / rx;
    float vy = (-y1p - cyp) / ry;
    float theta0 = std::atan2(uy, ux);
    float dTheta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && dTheta > 0.0f) {
        dTheta -= 2.0f * kPiSvgPath;
    } else if (sweep && dTheta < 0.0f) {
        dTheta += 2.0f * kPiSvgPath;
    }

    // <= 90 degrees per cubic segment (12.2.1).
    int segments = static_cast<int>(std::ceil(std::fabs(dTheta) / (kPiSvgPath * 0.5f) - 1e-4f));
    if (segments < 1) {
        segments = 1;
    }
    float step = dTheta / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        appendArcSegment(path, center, rx, ry, cosRot, sinRot, theta0 + static_cast<float>(i) * step, step);
    }
}

bool parseSvgPathData(std::string_view d, SvgPath& out) {
    Scanner sc{d, 0};
    char cmd = 0;
    char prevCmd = 0;
    Vec2 cur{0, 0};
    Vec2 subpathStart{0, 0};
    Vec2 lastCubicCtrl{0, 0};
    Vec2 lastQuadCtrl{0, 0};
    bool started = false;

    while (!sc.atEnd()) {
        size_t iterationStart = sc.i;
        if (isCommand(sc.s[sc.i])) {
            cmd = sc.s[sc.i];
            ++sc.i;
        } else if (cmd == 0) {
            return false;
        } else if (cmd == 'M') {
            cmd = 'L';  // An implicit repetition of moveto is a lineto.
        } else if (cmd == 'm') {
            cmd = 'l';
        }

        bool relative = cmd >= 'a' && cmd <= 'z';
        Vec2 origin = relative ? cur : Vec2{0, 0};
        char upper = relative ? static_cast<char>(cmd - ('a' - 'A')) : cmd;

        if (upper != 'M' && !started) {
            return false;  // Every path must start with a moveto.
        }

        switch (upper) {
            case 'M': {
                Vec2 p;
                if (!sc.point(p)) {
                    return false;
                }
                cur = add(origin, p);
                out.moveTo(cur);
                subpathStart = cur;
                started = true;
                break;
            }
            case 'L': {
                Vec2 p;
                if (!sc.point(p)) {
                    return false;
                }
                cur = add(origin, p);
                out.lineTo(cur);
                break;
            }
            case 'H': {
                float x;
                if (!sc.number(x)) {
                    return false;
                }
                cur = Vec2{origin.x + x, cur.y};
                out.lineTo(cur);
                break;
            }
            case 'V': {
                float y;
                if (!sc.number(y)) {
                    return false;
                }
                cur = Vec2{cur.x, origin.y + y};
                out.lineTo(cur);
                break;
            }
            case 'C': {
                Vec2 c1, c2, p;
                if (!sc.point(c1) || !sc.point(c2) || !sc.point(p)) {
                    return false;
                }
                c1 = add(origin, c1);
                c2 = add(origin, c2);
                p = add(origin, p);
                out.cubicTo(c1, c2, p);
                lastCubicCtrl = c2;
                cur = p;
                break;
            }
            case 'S': {
                Vec2 c2, p;
                if (!sc.point(c2) || !sc.point(p)) {
                    return false;
                }
                c2 = add(origin, c2);
                p = add(origin, p);
                // The first control point mirrors the previous one, or is the current point when
                // the previous command was not a cubic.
                bool reflect = prevCmd == 'C' || prevCmd == 'c' || prevCmd == 'S' || prevCmd == 's';
                Vec2 c1 =
                    reflect ? Vec2{2.0f * cur.x - lastCubicCtrl.x, 2.0f * cur.y - lastCubicCtrl.y} : cur;
                out.cubicTo(c1, c2, p);
                lastCubicCtrl = c2;
                cur = p;
                break;
            }
            case 'Q':
            case 'T': {
                Vec2 qc, p;
                if (upper == 'Q') {
                    if (!sc.point(qc) || !sc.point(p)) {
                        return false;
                    }
                    qc = add(origin, qc);
                } else {
                    if (!sc.point(p)) {
                        return false;
                    }
                    bool reflect = prevCmd == 'Q' || prevCmd == 'q' || prevCmd == 'T' || prevCmd == 't';
                    qc = reflect ? Vec2{2.0f * cur.x - lastQuadCtrl.x, 2.0f * cur.y - lastQuadCtrl.y} : cur;
                }
                p = add(origin, p);
                // Quadratic -> cubic: the controls sit two thirds of the way to the quad control.
                Vec2 c1{cur.x + 2.0f / 3.0f * (qc.x - cur.x), cur.y + 2.0f / 3.0f * (qc.y - cur.y)};
                Vec2 c2{p.x + 2.0f / 3.0f * (qc.x - p.x), p.y + 2.0f / 3.0f * (qc.y - p.y)};
                out.cubicTo(c1, c2, p);
                lastQuadCtrl = qc;
                cur = p;
                break;
            }
            case 'A': {
                float rx, ry, rot;
                bool largeArc, sweep;
                Vec2 p;
                if (!sc.number(rx) || !sc.number(ry) || !sc.number(rot) || !sc.flag(largeArc) ||
                    !sc.flag(sweep) || !sc.point(p)) {
                    return false;
                }
                p = add(origin, p);
                appendSvgArc(out, cur, rx, ry, rot, largeArc, sweep, p);
                cur = p;
                break;
            }
            case 'Z': {
                out.close();
                cur = subpathStart;
                break;
            }
            default:
                return false;
        }

        prevCmd = cmd;
        if (sc.i == iterationStart) {
            return false;  // No progress (e.g. "ZZ0"): reject rather than spin.
        }
    }
    return true;
}

}  // namespace xt::compile
