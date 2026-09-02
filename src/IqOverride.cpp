#include "IqOverride.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <cmath>
#include <cstring>

namespace {

bool nearlyEqual(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

// The mode strings rkaiq's IQ parser matches on. These are the calibration
// enums, which are a different set from the runtime uAPI ones.
const char* const kAeManual = "\"RK_AIQ_OP_MODE_MANUAL\"";
const char* const kAeAuto   = "\"RK_AIQ_OP_MODE_AUTO\"";
const char* const kWbManual = "\"CALIB_WB_MODE_MANUAL\"";
const char* const kWbAuto   = "\"CALIB_WB_MODE_AUTO\"";
const char* const kWbGain   = "\"CALIB_MWB_MODE_WBGAIN\"";
const char* const kWbCct    = "\"CALIB_MWB_MODE_CCT\"";

int skipWs(const QByteArray& t, int i)
{
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r'))
        ++i;
    return i;
}

// Index one past the JSON value that begins at i, or -1 if malformed.
int endOfValue(const QByteArray& t, int i)
{
    i = skipWs(t, i);
    if (i >= t.size())
        return -1;

    const char c = t[i];

    if (c == '"') {
        for (++i; i < t.size(); ++i) {
            if (t[i] == '\\') { ++i; continue; }
            if (t[i] == '"')  return i + 1;
        }
        return -1;
    }

    if (c == '{' || c == '[') {
        const char open = c;
        const char close = (c == '{') ? '}' : ']';
        int depth = 0;
        bool inString = false;
        for (; i < t.size(); ++i) {
            const char d = t[i];
            if (inString) {
                if (d == '\\') ++i;
                else if (d == '"') inString = false;
                continue;
            }
            if (d == '"') inString = true;
            else if (d == open) ++depth;
            else if (d == close && --depth == 0) return i + 1;
        }
        return -1;
    }

    // A number, or true/false/null.
    while (i < t.size() && std::strchr(",}] \t\r\n", t[i]) == nullptr)
        ++i;
    return i;
}

// Index just past the colon following "key", searched within [from, to).
int findMember(const QByteArray& t, const char* key, int from, int to)
{
    const QByteArray token = QByteArray("\"") + key + "\"";
    for (int i = from; i >= 0 && i < to; ) {
        i = t.indexOf(token, i);
        if (i < 0 || i >= to)
            return -1;
        const int colon = skipWs(t, i + token.size());
        if (colon < t.size() && t[colon] == ':')
            return colon + 1;
        i += token.size();
    }
    return -1;
}

// Walk a chain of keys, narrowing the region at each step.
bool locate(const QByteArray& t, const QVector<const char*>& path,
            int from, int to, int& valueStart, int& valueEnd)
{
    int regionStart = from;
    int regionEnd = to;

    for (const char* key : path) {
        const int after = findMember(t, key, regionStart, regionEnd);
        if (after < 0)
            return false;
        const int start = skipWs(t, after);
        const int end = endOfValue(t, start);
        if (end < 0)
            return false;
        regionStart = start;
        regionEnd = end;
    }

    valueStart = regionStart;
    valueEnd = regionEnd;
    return true;
}

// Replace the value a key chain points at. Returns the change in length so the
// caller can keep its enclosing region valid.
int replaceAt(QByteArray& t, const QVector<const char*>& path,
              const QByteArray& newValue, int from, int to)
{
    int start = 0;
    int end = 0;
    if (!locate(t, path, from, to, start, end))
        return 0;

    t.replace(start, end - start, newValue);
    return newValue.size() - (end - start);
}

// Multiply every occurrence of a numeric member within a region, keeping the
// shape of a tuning curve while changing its overall strength.
int scaleMembers(QByteArray& t, const char* key, double factor, int from, int to);

// Plain decimal, never scientific: the values are all small and human-scale,
// and keeping them ordinary keeps the file readable for anyone diffing it.
QByteArray number(double value)
{
    QByteArray out = QByteArray::number(value, 'f', 6);
    while (out.size() > 1 && out.endsWith('0'))
        out.chop(1);
    if (out.endsWith('.'))
        out.chop(1);
    return out;
}

int scaleMembers(QByteArray& t, const char* key, double factor, int from, int to)
{
    int delta = 0;
    int cursor = from;
    while (true) {
        const int after = findMember(t, key, cursor, to + delta);
        if (after < 0)
            break;
        const int start = skipWs(t, after);
        const int end = endOfValue(t, start);
        if (end < 0)
            break;

        const double original = t.mid(start, end - start).trimmed().toDouble();
        const QByteArray replacement = number(original * factor);
        t.replace(start, end - start, replacement);

        const int step = replacement.size() - (end - start);
        delta += step;
        cursor = start + replacement.size();
    }
    return delta;
}

} // namespace

bool IqOverride::Values::isNeutral() const
{
    return !aeManual && !awbManual
        && contrast == 128 && brightness == 128
        && saturation == 128 && hue == 128
        && nearlyEqual(sharpness, 50.0);
}

bool IqOverride::Values::operator==(const Values& other) const
{
    return aeManual == other.aeManual
        && awbManual == other.awbManual
        && wbByTemperature == other.wbByTemperature
        && colourTemperature == other.colourTemperature
        && nearlyEqual(exposureSeconds, other.exposureSeconds)
        && nearlyEqual(analogueGain, other.analogueGain)
        && nearlyEqual(ispDigitalGain, other.ispDigitalGain)
        && nearlyEqual(wbGainR, other.wbGainR)
        && nearlyEqual(wbGainGr, other.wbGainGr)
        && nearlyEqual(wbGainGb, other.wbGainGb)
        && nearlyEqual(wbGainB, other.wbGainB)
        && contrast == other.contrast
        && brightness == other.brightness
        && saturation == other.saturation
        && hue == other.hue
        && nearlyEqual(sharpness, other.sharpness);
}

IqOverride::~IqOverride()
{
    discardScratch();
}

void IqOverride::discardScratch()
{
    if (!m_scratchRoot.isEmpty())
        QDir(m_scratchRoot).removeRecursively();
    m_scratchRoot.clear();
    m_scratchDir.clear();
}

bool IqOverride::load(const QString& baseIqDir, const QString& iqFileName)
{
    m_original.clear();
    m_fileName = iqFileName;

    if (iqFileName.isEmpty()) {
        m_lastError = QStringLiteral("No IQ file name for this sensor");
        return false;
    }

    const QString path = QDir(baseIqDir).filePath(iqFileName);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }

    m_original = file.readAll();
    if (m_original.isEmpty()) {
        m_lastError = QStringLiteral("%1 is empty").arg(path);
        return false;
    }

    if (m_original.indexOf("\"scene_isp30\"") < 0) {
        m_lastError = QStringLiteral("%1 has no scene_isp30 section").arg(path);
        m_original.clear();
        return false;
    }

    return true;
}

// Patches one scene in place. The edits are textual rather than a parse and
// re-serialise because rkaiq's IQ parser is order-sensitive: writing the file
// back out through a JSON library that sorts its keys reorders every object and
// makes the parser reject sections it previously accepted (it reports things
// like "ACCM:E:check yalp-gains[12]"). Editing only the values that change
// leaves every other byte, and the key order, exactly as shipped.
int IqOverride::patchScene(QByteArray& text, int sceneStart, int sceneEnd,
                           const Values& values) const
{
    int end = sceneEnd;
    const auto apply = [&](const QVector<const char*>& path, const QByteArray& value) {
        end += replaceAt(text, path, value, sceneStart, end);
    };

    apply({"ae_calib", "CommCtrl", "AecOpType"},
          QByteArray(values.aeManual ? kAeManual : kAeAuto));

    const QVector<const char*> linear {"ae_calib", "CommCtrl", "AecManualCtrl", "LinearAE"};
    const auto aeField = [&](const char* leaf, const QByteArray& value) {
        QVector<const char*> path = linear;
        path.append(leaf);
        apply(path, value);
    };
    aeField("ManualTimeEn", "1");
    aeField("ManualGainEn", "1");
    aeField("ManualIspDgainEn", "1");
    aeField("TimeValue", number(values.exposureSeconds));
    aeField("GainValue", number(values.analogueGain));
    aeField("IspDGainValue", number(values.ispDigitalGain));

    apply({"wb_v21", "control", "mode"},
          QByteArray(values.awbManual ? kWbManual : kWbAuto));
    apply({"wb_v21", "manualPara", "mode"},
          QByteArray(values.wbByTemperature ? kWbCct : kWbGain));

    // Both forms are written every time: only the mode above decides which
    // rkaiq reads, and leaving the other stale makes a mode switch surprising.
    const QByteArray gains = "[" + number(values.wbGainR) + "," + number(values.wbGainGr)
                           + "," + number(values.wbGainGb) + "," + number(values.wbGainB) + "]";
    apply({"wb_v21", "manualPara", "cfg", "mwbGain"}, gains);
    apply({"wb_v21", "manualPara", "cfg", "cct", "CCT"},
          QByteArray::number(values.colourTemperature));

    // Colour processing maps one-to-one onto the UI's four sliders.
    apply({"cproc", "param", "enable"}, "1");
    apply({"cproc", "param", "brightness"}, QByteArray::number(values.brightness));
    apply({"cproc", "param", "contrast"}, QByteArray::number(values.contrast));
    apply({"cproc", "param", "saturation"}, QByteArray::number(values.saturation));
    apply({"cproc", "param", "hue"}, QByteArray::number(values.hue));

    // Sharpening has no single knob: it is tuned per ISO across two sensor
    // modes. Scaling every sharp_ratio keeps the tuned curve's shape and moves
    // its overall strength, with 50 leaving the file exactly as shipped.
    int sharpStart = 0;
    int sharpEnd = 0;
    if (locate(text, {"sharp_v4"}, sceneStart, end, sharpStart, sharpEnd)) {
        const double factor = values.sharpness / 50.0;
        if (!nearlyEqual(factor, 1.0))
            end += scaleMembers(text, "sharp_ratio", factor, sharpStart, sharpEnd);
    }

    return end - sceneEnd;
}

QString IqOverride::write(const Values& values)
{
    if (!isLoaded()) {
        m_lastError = QStringLiteral("No IQ file loaded to override");
        return {};
    }

    QByteArray text = m_original;

    // Which scene rkaiq selects depends on preInit_scene, so every scene is
    // patched rather than guessing at the one that will be used.
    int patched = 0;
    int searchFrom = 0;
    while (true) {
        const int keyIndex = text.indexOf("\"scene_isp30\"", searchFrom);
        if (keyIndex < 0)
            break;

        const int colon = skipWs(text, keyIndex + int(strlen("\"scene_isp30\"")));
        if (colon >= text.size() || text[colon] != ':')
            break;

        const int start = skipWs(text, colon + 1);
        const int end = endOfValue(text, start);
        if (end < 0)
            break;

        const int delta = patchScene(text, start, end, values);
        ++patched;
        searchFrom = end + delta;
    }

    if (patched == 0) {
        m_lastError = QStringLiteral("IQ file has no scene_isp30 section to patch");
        return {};
    }

    // A fresh directory per write, because rkaiq caches parsed calibration
    // against the file path: writing new values to the same path leaves it
    // using the first version it ever read, so only the first change lands.
    const QString root = QDir(QDir::tempPath())
                             .filePath(QStringLiteral("camera-ctls-iq-%1")
                                           .arg(QCoreApplication::applicationPid()));
    const QString dir = QDir(root).filePath(QString::number(++m_generation));
    if (!QDir().mkpath(dir)) {
        m_lastError = QStringLiteral("Cannot create %1").arg(dir);
        return {};
    }

    const QString previous = m_scratchDir;
    m_scratchDir = dir;
    m_scratchRoot = root;

    // rkaiq re-derives the file name from the module info, so the copy has to
    // keep the original name and only the directory may differ.
    const QString path = QDir(m_scratchDir).filePath(m_fileName);
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = QStringLiteral("Cannot write %1: %2").arg(path, out.errorString());
        return {};
    }
    out.write(text);
    out.close();

    // The engine has been handed the new directory, so the one it used before
    // is no longer referenced.
    if (!previous.isEmpty() && previous != dir)
        QDir(previous).removeRecursively();

    return m_scratchDir;
}
