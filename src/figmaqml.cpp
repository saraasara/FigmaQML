#include "figmadocument.h"
#include "figmaparser.h"
#include "figmaqml.h"
#include "fontcache.h"
#include "fontinfo.h"
#include "utils.h"
#include <QVersionNumber>
#include <QTimer>
#include <QSaveFile>
#include <QSize>
#include <QQmlEngine>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QStandardPaths>
#ifdef USE_NATIVE_FONT_DIALOG
#include <QFontDialog>
#include <QApplication>
#endif


#ifdef HAS_QUL
extern void executeQulApp(const QVariantMap& parameters, const FigmaQml& figmaQml, const std::vector<int>& elements);
extern bool writeQul(const QString& path, const QVariantMap& parameters, const FigmaQml& figmaQml, bool writeAsApp, const std::vector<int>& elements);
#endif

#include <QTime>
#define TIMED_START(s)  const auto s = QTime::currentTime();
#define TIMED_END(s, p) if(m_flags & Timed ) {emit info(toStr("timed", p, s.msecsTo(QTime::currentTime())));}

#define SCAT(a, b) a ## b
#define SCAT2(a, b) SCAT(a, b)
#define RAII(x) RAII_ SCAT2(raii, __LINE__)(x)

using namespace std::chrono_literals;

const QLatin1String qmlViewPath("/qml/");
//why there were two folders? onst QLatin1String sourceViewPath("/sources/");
const QLatin1String Images("/images/");
const QLatin1String FileHeader("//Generated by FigmaQML %1\n\n");

static int levenshteinDistance(const QString& s1, const QString& s2) {
    const auto l1 = s1.length();
    const auto l2 = s2.length();

    auto dist = std::vector<std::vector<int>>(l2 + 1, std::vector<int>(l1 + 1));


    for(auto i = 0; i <= l1 ; i++) {
       dist[0][i] = i;
    }

    for(auto j = 0; j <= l2; j++) {
       dist[j][0] = j;
    }
    for (auto j = 1; j <= l1; j++) {
       for(auto i = 1; i <= l2 ;i++) {
          const auto track = (s2[i-1] == s1[j-1]) ? 0 : 1;
          const auto t = std::min((dist[i - 1][j] + 1), (dist[i][j - 1] + 1));
          dist[i][j] = std::min(t, (dist[i - 1][j - 1] + track));
       }
    }
    return dist[l2][l1];
}

enum Format {
    None = 0, JPEG, PNG
};

FigmaQml::~FigmaQml() {
}

int FigmaQml::canvasCount() const {
    return m_uiDoc ? m_uiDoc->size() : 0;
}

int FigmaQml::elementCount() const {
    return m_uiDoc && canvasCount() > 0 ? m_uiDoc->current().size() : 0;
}

int FigmaQml::currentElement() const {
    return (m_uiDoc && !m_uiDoc->empty()) ? m_uiDoc->current().currentIndex() : 0;
}

int FigmaQml::currentCanvas() const {
    return m_uiDoc ? m_uiDoc->currentIndex() : 0;
}

QString FigmaQml::canvasName() const {
    return m_uiDoc ? m_uiDoc->current().name() : "";
}

QString FigmaQml::documentName() const {
    return m_uiDoc ? m_uiDoc->name() : "";
}

QString FigmaQml::qmlDir() const {
    return m_qmlDir;
}

QString FigmaQml::elementName() const {
    return (m_uiDoc && !m_uiDoc->empty()) ? m_uiDoc->current().name(currentElement()) : QString();
}


QString FigmaQml::documentsLocation() const {
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

const auto FrameDelay = 500ms;

bool FigmaQml::setCurrentElement(int current) {
    if(current < 0 || current >= elementCount())
        return false;
    if(current != currentElement()) {
        m_uiDoc->getCurrent()->setCurrent(current);
        emit elementNameChanged();
        QTimer::singleShot(FrameDelay, this, [this](){emit currentElementChanged();}); //delayed
    }
    return true;
}

bool FigmaQml::setCurrentCanvas(int current) {
    if(current < 0 || current >= canvasCount())
        return false;
    if(current != currentCanvas()) {
        m_uiDoc->setCurrent(current);
        if(m_uiDoc->currentIndex() >= m_uiDoc->current().size()) {
            m_uiDoc->getCurrent()->setCurrent(m_uiDoc->current().size() - 1);
        }
        emit currentCanvasChanged();
        emit elementNameChanged();
        emit elementCountChanged();
        QTimer::singleShot(FrameDelay, this, [this](){emit currentElementChanged();}); //delayed
    }
    return true;
}



QString FigmaQml::validFileName(const QString& name) {
   return FigmaParser::makeFileName(name);
}

bool FigmaQml::saveAllQML(const QString& folderName) {
#ifdef Q_OS_WINDOWS
    QDir d(folderName.startsWith('/') ? folderName.mid(1) : folderName);
#else
    QDir d(folderName);
#endif
    if(!ensureDirExists(d.absolutePath())) {
        return false;
    }
    QSet<QString> componentNames;
    for(const auto& c : *m_sourceDoc) {
        for(const auto& e : *c) {
            const auto sourceName = FigmaParser::makeFileName(c->name());
            const auto fullname = QString("%1/%2_%3.qml").arg(d.absolutePath(), sourceName, e->name());
            QSaveFile file(fullname);
            if(!file.open(QIODevice::WriteOnly)) {
                emit error(QString("Failed to write %1 %2 %3 %4").arg(file.errorString(), fullname, d.absolutePath(), e->name()));
                return false;
            }
            if(e->data().length() == 0 || file.write(e->data()) < 0) {
                emit error(QString("Failed to write %1 %2 %3 %4").arg(file.errorString(), fullname, d.absolutePath(), e->name()));
                return false;
            }
            const auto elementComponents = m_sourceDoc->components(e->name());
            componentNames.unite(QSet(elementComponents.begin(), elementComponents.end()));
            file.commit();
        }
    }

    for(const auto& componentName : componentNames) {
        Q_ASSERT(componentName.endsWith(FIGMA_SUFFIX));
        const auto fullname = QString("%1/%2.qml").arg(d.absolutePath(), componentName);
        QSaveFile file(fullname);
        if(!file.open(QIODevice::WriteOnly)) {
            emit error(QString("Failed to write \"%1\" \"%2\" \"%3\" \"%4\"").arg(file.errorString(), fullname, d.absolutePath(), componentName));
            return false;
        }

        if(!m_sourceDoc->containsComponent(componentName)) {
            emit error(QString("Failed to find \"%1\" on write").arg(componentName));
            return false;
        }

        const auto cd = m_sourceDoc->component(componentName);
        if(cd.length() == 0 || file.write(cd) < 0) {
            emit error(QString("Failed to write \"%1\" \"%2\" \"%3\" \"%4\"").arg(file.errorString(), fullname, d.absolutePath(), componentName));
            return false;
        }
        file.commit();
    }

    if(!saveImages(d.absolutePath() + Images))
        return false;
    emit info(QString("%1 files written into %2").arg(m_imageFiles.size() + componentNames.count()
                                                      + std::accumulate(m_sourceDoc->begin(), m_sourceDoc->end(), 0, [](const auto &a, const auto& c){return a + c->size();}))
              .arg(d.absolutePath()));
    return true;
}

QUrl FigmaQml::element() const {
    return (m_uiDoc && !m_uiDoc->empty()) ?  QUrl::fromLocalFile(QString(m_uiDoc->current().data())) : QUrl();
}

QByteArray FigmaQml::sourceCode() const {
    return (m_sourceDoc && !m_sourceDoc->empty()) ?  m_sourceDoc->current().data() : QByteArray();
}

FigmaQml::FigmaQml(const QString& qmlDir, const QString& fontFolder, FigmaProvider& provider, QObject *parent) : QObject(parent),
    m_qmlDir(qmlDir), mProvider(provider), m_imports(defaultImports()), m_fontCache(std::make_unique<FontCache>()), m_fontFolder(fontFolder),
    m_fontInfo{ new FontInfo{this} } {
    qmlRegisterUncreatableType<FigmaQml>("FigmaQml", 1, 0, "FigmaQml", "");
    QObject::connect(this, &FigmaQml::currentElementChanged, this, [this]() {
        if(!m_uiDoc) {
            emit error("Invalid element!");
            return;
        }
        Q_ASSERT(m_sourceDoc);
        m_sourceDoc->getCurrent()->setCurrent(m_uiDoc->getCurrent()->currentIndex());
    });
    QObject::connect(this, &FigmaQml::currentCanvasChanged, this, [this]() {
        m_sourceDoc->setCurrent(m_uiDoc->currentIndex());
    });
    QObject::connect(this, &FigmaQml::sourceCodeChanged, this, &FigmaQml::elementChanged);
    QObject::connect(this, &FigmaQml::currentElementChanged, this, &FigmaQml::sourceCodeChanged);
    QObject::connect(this, &FigmaQml::currentCanvasChanged, this, &FigmaQml::sourceCodeChanged);
    QObject::connect(this, &FigmaQml::currentElementChanged, this, &FigmaQml::elementNameChanged);
    QObject::connect(this, &FigmaQml::currentElementChanged, this, &FigmaQml::componentsChanged);
    QObject::connect(this, &FigmaQml::currentCanvasChanged, this, &FigmaQml::canvasNameChanged);
    QObject::connect(this, &FigmaQml::imageDimensionMaxChanged, this, [this]() {
        if(m_imageDimensionMax <= 0) {
            m_imageDimensionMax = 1024;
        }
    });


#ifdef Q_OS_LINUX
    QObject::connect(m_fontInfo, &FontInfo::fontPath, this, &FigmaQml::fontPathFound);
    QObject::connect(m_fontInfo, &FontInfo::pathError, this, &FigmaQml::fontPathError);
#endif
    const auto fontFolderChanged = [this]() {
        const QDir fontFolder(m_fontFolder);
        if(!fontFolder.exists())
            emit warning(QString("Folder \"%1\", not found").arg(m_fontFolder));
        for(const auto& entry : fontFolder.entryInfoList()) {
            int id = -1;
            if(!entry.isDir() && !entry.fileName().endsWith(".txt") &&  (id = QFontDatabase::addApplicationFont(entry.absoluteFilePath())) < 0)
                emit warning(QString("Font \"%1\", cannot be loaded").arg(entry.absoluteFilePath()));
            else {
                const auto families = QFontDatabase::applicationFontFamilies(id);
                if(!families.isEmpty()) {
                    QFont font(families);
                    emit info("font \"" + entry.fileName() + "\" loaded");
                    qDebug() << "Font" << entry.absoluteFilePath() << "loaded "<< font;
                    emit fontLoaded(font);
                }
             }
        }
    };

    QObject::connect(this, QOverload<FigmaFileDocument*>::of(&FigmaQml::figmaDocumentCreated), this, [this](FigmaFileDocument* doc) {
        Q_ASSERT(doc->type() == FigmaFileDocument::type());
        Q_ASSERT(!m_uiDoc);
        if(doc) {
            m_uiDoc.reset(doc);
            emit isValidChanged();
            emit canvasCountChanged();
            emit elementCountChanged();
            emit documentNameChanged();
            emit elementChanged();
            emit fontsChanged();
        } else {
            emit error("Invalid document");
        }
        if(mRestore)
            mRestore(doc);
        mRestore = nullptr;
    });

    QObject::connect(this, &FigmaQml::error, this, [this](const QString&) {
        if(mRestore)
            mRestore(false);
        mRestore = nullptr;
    });

    QObject::connect(this, QOverload<FigmaDataDocument*>::of(&FigmaQml::figmaDocumentCreated), this, [this](FigmaDataDocument* doc) {
        Q_ASSERT(doc->type() == FigmaDataDocument::type());
        Q_ASSERT(!m_sourceDoc);
        if(doc) {
            m_sourceDoc.reset(doc);
            emit sourceCodeChanged();
            emit documentCreated();
        } else {
            emit error("Invalid document");
        }
    });

    QObject::connect(this, &FigmaQml::flagsChanged, this, &FigmaQml::updateDefaultImports);

    QObject::connect(this, &FigmaQml::fontFolderChanged, this, fontFolderChanged);
    fontFolderChanged();


    QObject::connect(this, &FigmaQml::canvasCountChanged, this, &FigmaQml::elementsChanged);
    QObject::connect(this, &FigmaQml::elementCountChanged, this, &FigmaQml::elementsChanged);
    QObject::connect(this, &FigmaQml::sourceCodeChanged, this, &FigmaQml::elementsChanged);

}


void FigmaQml::updateDefaultImports() {
    const auto di = defaultImports();
    const QStringList to_check = {"Qt5Compat.GraphicalEffects"};
    bool has_changes = false;
    for(const auto& c : to_check) {
        if(di.contains(c) && !m_imports.contains(c)) {
            m_imports.insert(c, "");
            has_changes = true;
        }
        else if(!di.contains(c) && m_imports.contains(c)) {
            m_imports.remove(c);
            has_changes = true;
        }
    }
    if(has_changes) {
        emit importsChanged();
    }
}

QVariantList FigmaQml::elements() const {
    QVariantList el_list;
    int canvas_index = 0;
    if(!m_sourceDoc)
        return el_list;
    for(const auto& canvas : *m_sourceDoc) {
        int element_index = 0;
        for(const auto& element : *canvas) {
            QVariantMap map{
                {"canvas", canvas_index},
                {"element", element_index},
                {"canvas_name", canvas->name()},
                {"element_name", element->name()}
            };
            el_list.append(std::move(map));
            ++element_index;
        }
        ++canvas_index;
    }
    return el_list;
}

QVariantMap FigmaQml::defaultImports() const {
    return
#ifdef QT5
        {{"QtQuick", QString("2.15")},
        {"QtGraphicalEffects", QString("1.15")},
         {"QtQuick.Shapes", QString("1.15")}};
#else
        (m_flags & QulMode) ?
            QVariantMap{
            {"QtQuick", QString("")},
            {"QtQuick.Shapes", QString("")}
        } : QVariantMap{
            {"QtQuick", QString("")},
            {"Qt5Compat.GraphicalEffects", QString("")},
            {"QtQuick.Shapes", QString("")}
         };
#endif

}

bool FigmaQml::setBrokenPlaceholder(const QString &placeholder) {
    QFile file(placeholder);
    if(!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    m_brokenPlaceholder = "data:image/jpeg;base64," + file.readAll().toBase64();
    return true;
}


bool FigmaQml::isValid() const {
    return m_uiDoc && !m_uiDoc->empty();
}

QStringList FigmaQml::components() const {
    if(m_sourceDoc && !m_sourceDoc->empty()) {
        return components(m_sourceDoc->currentIndex(), m_sourceDoc->current().currentIndex());
    }
    return {};
}


QStringList FigmaQml::components(int canvas_index, int element_index) const {
    if(m_sourceDoc && !m_sourceDoc->empty()) {
        const auto key = (*(m_sourceDoc->begin() + canvas_index))->name(element_index);
        const auto component_list = m_sourceDoc->components(key);
        for(const auto& c : component_list) {
            const auto name_s = qmlTargetDir() + '/' + c + ".qml";
#if 0            // look for this solution, have element subcomponents in their own folders...
            const auto name_l = qmlTargetDir() + '/' + c + ".qml";
            Q_ASSERT(QFileInfo::exists(name_l) || QFileInfo::exists(name_s));
#else
            Q_ASSERT(QFileInfo::exists(name_s));
#endif
            Q_ASSERT(!m_sourceDoc->component(c).isEmpty());
            Q_ASSERT(!m_sourceDoc->componentObject(c).isEmpty());
        }
        return component_list;
    } else {
        return QStringList();
    }
}

QByteArray FigmaQml::componentSourceCode(const QString &name) const {
    return (!name.isEmpty()) && m_sourceDoc && m_sourceDoc->containsComponent(name) ? m_sourceDoc->component(name) : QByteArray();
}

QString FigmaQml::componentObject(const QString &name) const {
    return (!name.isEmpty()) && m_sourceDoc && m_sourceDoc->containsComponent(name) ? m_sourceDoc->componentObject(name) : QString();
}


void FigmaQml::cancel() {
    emit cancelled();
}

void FigmaQml::doCancel() {
    m_doCancel = true;
}

void FigmaQml::setFilter(const QMap<int, QSet<int>>& filter) {
    m_filter = filter;
}

QByteArray FigmaQml::prettyData(const QByteArray& data) const {
    if(data.isEmpty()) {
        //emit const_cast<FigmaQml*>(this)->error("No data");
        return QString("No data").toLatin1();
    }
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(data, &error);
    if(error.error != QJsonParseError::NoError) {
       return QString("JSON parse error: %1 at %2\n\n")
                .arg(error.errorString())
                .arg(error.offset).toLatin1() + data;
    }
    const auto bytes = json.toJson();
    return bytes;
}

// folder is more of prefix...
std::optional<QStringList> FigmaQml::saveImages(const QString &folder, const QSet<QString>& filter) const {
    if(!ensureDirExists(folder))
        return std::nullopt;
    QStringList img_list;
    for(const auto& [k, i] : m_imageFiles.asKeyValueRange()) {
        if(!filter.empty()) {
            if(m_imageContexts.contains(k)) {
                const auto awhat = m_imageContexts[k];
                bool found = false;
                for(const auto& f : filter)
                    if(awhat.contains(f))
                        found = true;
                if(!found)
                     continue; // filter out
            }
        }
        const QFileInfo file(i.first + i.second);
        if(!file.exists()) {
            qDebug() << "invalid filename:" << file.absoluteFilePath() << "not found";
            emit error(QString("Invalid filename: %1 (not found)").arg(file.absoluteFilePath()));
            return std::nullopt;
        }
        const auto target = folder + file.fileName();
        img_list.append(target);
        const QFile targetEntry(target);
        if(targetEntry.exists(target)) {
            if(targetEntry.size() == file.size()) {
                // I dont fully get what this means.... how they can be same, error elsewhere?
                emit warning(QString("Are equal %1 to %2").arg(file.absoluteFilePath(), target));
                continue;
             } else {
                emit error(QString("Cannot replace %1 to %2").arg(file.absoluteFilePath(), target));
                return std::nullopt;
            }
        }
        if(!QFile::copy(file.absoluteFilePath(), target)) {
            emit error(QString("Cannot copy %1 to %2").arg(file.absoluteFilePath(), target));
            return std::nullopt;
        }
    }
    return std::make_optional(img_list);
}

void FigmaQml::addImageFile(const QString& imageRef, bool isRendering) {
    if(isRendering){
        mProvider.getRendering(imageRef);
        QObject::connect(&mProvider, &FigmaProvider::imageReady, this, [this](const QString& imageRef, const QByteArray& bytes, int format) {
            addImageFileData(imageRef, bytes, format);
    });
    } else {
        mProvider.getImage(imageRef, QSize(m_imageDimensionMax, m_imageDimensionMax));
        QObject::connect(&mProvider, &FigmaProvider::imageReady, this, [this](const QString& imageRef, const QByteArray& bytes, int format) {
            addImageFileData(imageRef, bytes, format);
    });
    }
}

bool FigmaQml::addImageFileData(const QString& imageRef, const QByteArray& bytes, int mime) {
    //qDebug() << "FOO: addImageFileData" << imageRef;
    if(bytes.isEmpty())
        return false;

    const auto path = qmlTargetDir() + Images.mid(1);
    int count = 1;
    static const QRegularExpression re(R"([\\\/:*?"<>|\s;])");
    auto name = imageRef;
    name.replace(re, QLatin1String("_"));
    Q_ASSERT(mime == PNG || mime == JPEG);
    const QString extension = mime == JPEG ? "jpg" : "png";
    auto imageName = QString("%1.%2").arg(name, extension);
    while(QFile::exists(imageName)) {
        imageName = QString("%1_%2.%3").arg(name).arg(count).arg(extension);
        ++count;
        }
    ensureDirExists(path);
    const auto filename = path + imageName;
    QSaveFile file(filename);
    if(!file.open(QIODevice::WriteOnly)) {
        emit warning("error when write:" + imageRef + " " +  filename + " " + file.errorString());
        return false;
    }
    //qDebug() << "image saved" << imageRef << filename;
    file.write(bytes);
    file.commit();
    m_imageFiles.insert(imageRef, {path, imageName});
    return true;
}

bool FigmaQml::ensureDirExists(const QString& e) const {
     QDir dir(e);
     if(!dir.mkpath(".")) {
         emit error(QString("Cannot use dir %1").arg(dir.absolutePath()));
         return false;
     }
     //qDebug() << "Foo" << e << "is ok";
     return true;
}

QVariantMap FigmaQml::fonts() const {
    QVariantMap map;
    const auto c = m_fontCache->content();
    for(const auto& v : c)
        map.insert(v.first, v.second);
    return map;
}

void FigmaQml::setFonts(const QVariantMap& map) {
  const auto keys = map.keys();
  for (const auto &k : keys) {
    m_fontCache->insert(k, map[k].toString());
  }
}

template<class FigmaDocType>
void FigmaQml::createDocument(const QJsonObject& json) {
    m_state = State::Suspend;
    m_busy = true;
    emit busyChanged();
    auto ctimer = new QTimer(this);
    QObject::connect(ctimer, &QTimer::timeout, this, [ctimer, this, json](){
        if(m_state == State::Suspend) {
            if(mProvider.isReady()) {
                m_state = State::Constructing;

                auto doc = std::make_unique<FigmaDocType>(qmlTargetDir(), FigmaParser::name(json));
                if(doCreateDocument(*doc, json)) {
                    ctimer->stop();
                    ctimer->deleteLater();
                    Q_ASSERT(FigmaDocType::type() == doc->type());
                    emit figmaDocumentCreated(doc.release());
                } else if(m_state != State::Suspend) {
                    parseError(FigmaParser::lastError(), true);
                }
                if(m_state != State::Suspend) {
                    m_busy = false;
                    emit busyChanged();
                }
            }
        } else {
            ctimer->stop();
            ctimer->deleteLater();
            emit figmaDocumentCreated(static_cast<FigmaDocType*>(nullptr));
        }
    });
    ctimer->start(500);
}

QString FigmaQml::qmlTargetDir() const {
    return m_qmlDir + qmlViewPath;
}


void FigmaQml::createDocumentView(const QByteArray &data, bool restoreView) {

    if(mRestore)
        return;
    const auto json = object(data);
    if(!json)
        return;
    cleanDir(m_qmlDir);
    m_imageFiles.clear();
    m_uiDoc.reset();
    if(!restoreView)
        m_fontCache->clear();
    emit isValidChanged();
    emit canvasCountChanged();
    emit elementCountChanged();
    emit documentNameChanged();

    m_embedImages = true;

    const auto restoredCanvas = currentCanvas();
    const auto restoredElement = currentElement();

    mRestore = [this, restoreView, restoredElement, restoredCanvas, data](bool has_doc){
        if(has_doc)
             createDocumentSources(data);
        if(restoreView) {
            if(setCurrentCanvas(restoredCanvas))
                setCurrentElement(restoredElement);
        }
    };

    createDocument<FigmaFileDocument>(*json);
}


void FigmaQml::setFontMapping(const QString& key, const QString& value) {
    qDebug() << "set font" << key << "->" << value;
    m_fontCache->insert(key, value);
    emit refresh();
    emit fontsChanged();
}

void FigmaQml::resetFontMappings() {
    m_fontCache->clear();
    emit refresh();
    emit fontsChanged();
}


void FigmaQml::createDocumentSources(const QByteArray &data) {
    const auto json = object(data);
    if(!json)
        return;

    m_sourceDoc.reset();
    m_embedImages = m_flags & EmbedImages;

    createDocument<FigmaDataDocument>(*json);

}

void FigmaQml::restore(int flags, const QVariantMap& imports) {
    m_flags = flags;
    m_imports = imports;
}

void FigmaQml::cleanDir(const QString& dirName) {
    QDir dir(dirName);
    const auto entries = dir.entryInfoList();
    for(const auto& e : entries) {
        if(e.isFile() && !dir.remove(e.fileName()))
            emit error(toStr("Cannot remove", e.fileName()));
    }
}

std::optional<QJsonObject> FigmaQml::object(const QByteArray &data) {
    if(data.isEmpty())
        return std::nullopt;

    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(data, &parseError);
    if(parseError.error != QJsonParseError::NoError) {
       emit this->error(QString("When reading JSON: %1 at %2")
                .arg(parseError.errorString())
                .arg(parseError.offset));
        return std::nullopt;
    }

    if(!json.isObject()) {
        emit error("Object expected");
        return std::nullopt;
    }
    return json.object();
}

bool FigmaQml::busy() const {
    return m_busy;
}

QString FigmaQml::nearestFontFamily(const QString& requestedFont, bool useAlt) {
    if(!useAlt) {
        const QFont font(requestedFont);
        const QFontInfo fontInfo(font);  //this return mapped family
        const auto value = fontInfo.family();
        return value;
    } else {
#ifdef QT5
        QFontDatabase fdb;
        const QStringList fontFamilies = fdb.families();
#else
        const QStringList fontFamilies = QFontDatabase::families();
#endif
        int min = std::numeric_limits<int>::max();
        int index = -1;
        for(auto ff = 0; ff < fontFamilies.size() ; ff++) {
            const auto distance = levenshteinDistance(fontFamilies[ff], requestedFont);
            if(distance < min) {
                index = ff;
                min = distance;
            }
        }
        if(index < 0) {
            return requestedFont;
        }
        const auto value = fontFamilies[index];
        return value;
    }
}

void FigmaQml::setSignals(bool allow) {
    blockSignals(!allow);
}

void FigmaQml::parseError(const QString& str, bool isFatal) {
    if(m_state != State::Suspend) {
        if(!m_doCancel) {
            if(isFatal) {
                m_ok = false;
                emit error(str);
            } else
                emit warning(str);
        }
    }
}

std::optional<std::tuple<QByteArray, int>> FigmaQml::getImage(const QString& imageRef, bool isRendering) {
    if(isRendering) {
        const auto imageData = mProvider.cachedRendering(imageRef);
        if(imageData)
            return imageData;
        mProvider.getRendering(imageRef);
    } else {
        const auto imageData = mProvider.cachedImage(imageRef);
        if(imageData)
            return imageData;
        mProvider.getImage(imageRef, QSize(m_imageDimensionMax, m_imageDimensionMax));
    }
    return std::nullopt;
}

void FigmaQml::suspend() {
    m_state = State::Suspend;
}

QByteArray FigmaQml::imageData(const QString& imageRef, bool isRendering) {
    if(!m_ok || m_doCancel)
        return QByteArray();
    if(imageRef == FigmaParser::PlaceHolder)
        return m_brokenPlaceholder;
    else {
        if(m_embedImages) {
            const auto imageData = getImage(imageRef, isRendering);
            if(!imageData) {
                suspend();
                return{};
            }
            const auto& [bytes, mime] = imageData.value();
            if(bytes.isEmpty())
                return QByteArray();
            Q_ASSERT(mime == JPEG || mime == PNG);
            const QByteArray mimeString = mime == JPEG ? "jpeg" : "png";
            return "data:image/" + mimeString + ";base64," + bytes.toBase64();
        } else {
            if(!m_imageFiles.contains(imageRef)) {
                const auto imageData = getImage(imageRef, isRendering);
                if(!imageData) {
                    suspend();
                    return{};
                }
                const auto& [bytes, mime] = imageData.value();
                if(!addImageFileData(imageRef, bytes, mime))
                    return {};
            }
            return (Images.mid(1) +  m_imageFiles[imageRef].second).toLatin1();
        }
    }
}

QByteArray FigmaQml::nodeData(const QString& id) {
    if(!m_ok || m_doCancel)
        return QByteArray();
    const auto node = mProvider.cachedNode(id);
    if(!node) {
        mProvider.getNode(id);
        suspend();
        return {};
    }
    return *node;
}

QString FigmaQml::fontInfo(const QString& requestedFont) {
    if(m_flags & KeepFigmaFontName)
        return requestedFont;
    if(m_fontCache->contains(requestedFont))
        return (*m_fontCache)[requestedFont];
    const auto value = nearestFontFamily(requestedFont, m_flags & AltFontMatch);
    m_fontCache->insert(requestedFont, value);
    return value;
}

bool FigmaQml::writeQmlFile(const QString& component_name, const QByteArray& element_data, const QByteArray& header, const QString& subFolder) const {
    Q_ASSERT(subFolder.isEmpty()); // this is for future thinking, it is very bad that components get overwritten!
    Q_ASSERT(component_name.endsWith(FIGMA_SUFFIX));
    Q_ASSERT(!element_data.isEmpty());
    Q_ASSERT(!header.isEmpty());
    const QString filename = qmlTargetDir() + (!subFolder.isEmpty() ? subFolder + '/' : QString{}) + component_name + ".qml";
    if(QFile::exists(filename)) {
        //return true
        //TODO qDebug() << "already there!" << filename;
    }
    QDir().mkpath((QFileInfo(filename).path()));
    QSaveFile componentFile(filename);
    if(!componentFile.open(QIODevice::WriteOnly)) {
        emit error(toStr("Cannot write", filename, componentFile.errorString()));
        return false;
    }
    componentFile.write(header + element_data);
    componentFile.commit();
    return true;
}


bool FigmaQml::writeComponents(FigmaDocument& doc, const FigmaParser::Components& components, const QByteArray& header) {
    qDebug() << "write componets!";
    for(const auto& c : components) {

      const auto component_opt = FigmaParser::component(c->object(), m_flags, *this, components);
      if(!m_ok || m_doCancel || !component_opt)
          return false;
      const auto& component = component_opt.value();
      if(component.data().isEmpty()) {
          emit error(toStr("Invalid component", component.name()));
          return false;
      }

      const auto images = component.imageContexts();
      for(const auto& im : images) {
          if(!m_imageContexts.contains(im))
              m_imageContexts.insert(im, {});
          m_imageContexts[im].insert(components[component.id()]->name());
      }

      doc.addComponent(components[component.id()]->name(),
              components[component.id()]->object(), header + component.data());


      QStringList componentNames;
      for(const auto& id : component.components()) {
          Q_ASSERT(components.contains(id)); //just check here
          const auto compname = components[id]->name();
          componentNames.append(compname);
      }

      const auto subs = component.subComponents();
      for(const auto& [sub_name, sub_data] : subs.asKeyValueRange()) {
          const auto data = header + std::get<QByteArray>(sub_data);
          doc.addComponent(sub_name, std::get<QJsonObject>(sub_data), data);
          //if(std::get<QString>(sub_data).isEmpty()) {
              if(!writeQmlFile(sub_name, data, header/*, c->name()*/)) {
                  emit error(toStr("Cannot write sub component", sub_name, " for ", component.name()));
                  return false;
              }
          //}
      }


      if(!writeQmlFile(c->name(), component.data(), header)) {
          emit error(toStr("Cannot write component", component.name()));
          return false;
      }

    }
    return true;
}


bool FigmaQml::setDocument(FigmaDocument& doc,
                           const FigmaParser::Canvases& canvases,
                           const FigmaParser::Components& components,
                           const QByteArray& header) {
    int currentCanvas = 0;

    int currentElement = 0;

    for(const auto& c : canvases) {
        ++currentCanvas;
        currentElement = 0;
        auto canvas = doc.addCanvas(c.name());

        const auto elements = c.elements();

        qDebug() << "write elements";

        for(const auto& f : elements) {
            if(m_state == State::Suspend)
                return false;
            if(m_doCancel)
                return false;
            bool hasElement = true;
            if(!m_filter.isEmpty()) {
                ++currentElement;
                const auto keys = m_filter.keys();
                if(!keys.contains(currentCanvas) || !m_filter[currentCanvas].contains(currentElement))
                    hasElement = false;
            }

            const auto element_opt = hasElement ? FigmaParser::element(f, m_flags, *this, components) : FigmaParser::Element();
            if(!element_opt)
                return false;
            const auto& element = element_opt.value();

            const auto images = element.imageContexts();
            for(const auto& im : images) {
                if(!m_imageContexts.contains(im))
                    m_imageContexts.insert(im, {});
                m_imageContexts[im].insert(element.name());
            }
            if(m_state == State::Suspend)
                return false;

            if(m_doCancel)
                return false;
            if(!m_ok) {
                return false;
            }
            if(!element.data().isEmpty())
                canvas->addElement(element.name(), header + element.data());
            else
                canvas->addElement(element.name(), header + "Text{text: \"filtered out\"}");
            QStringList componentNames;
            for(const auto& id : element.components()) {
                componentNames.append(components[id]->name());
            }

            // this is bit confusing, the component owned sub componets are written before this function is called,
            // but as element owned has to be called elsewhere it happens here. Whole this when is written and parsed
            // what is confusing
            for(const auto& [sub_name, sub_data] : element.subComponents().asKeyValueRange()) {
                componentNames.append(sub_name);
                const auto data = header + std::get<QByteArray>(sub_data);
                doc.addComponent(sub_name, std::get<QJsonObject>(sub_data), data);
                //if(std::get<QString>(sub_data).isEmpty()) {
                    if(!writeQmlFile(sub_name, data, header/*, element.name()*/))
                        return false;
                //}
            }
            doc.setComponents(element.name(), std::move(componentNames));
        }
    }
    return true;
}

bool FigmaQml::doCreateDocument(FigmaDocument& doc, const QJsonObject& json) {
    m_ok = true;
    m_doCancel = false; // uff UniqueConnection requires a member func
    const auto d = QObject::connect(this, &FigmaQml::cancelled, this,
                                    &FigmaQml::doCancel, Qt::UniqueConnection);

    RAII(([d](){QObject::disconnect(d);}));


    Q_ASSERT(m_imageDimensionMax > 0);

    // erase 1st
    QDir dir(qmlTargetDir());
    const auto entries = dir.entryList();
    if(!ensureDirExists(qmlTargetDir()))
       return false;

    const auto versionNumber = QString(STRINGIFY(VERSION_NUMBER));
    QByteArray header = QString(FileHeader).arg(versionNumber).toLatin1();
    const auto keys =  m_imports.keys();
    for(const auto& k : keys) {
#ifdef QT5
        const auto ver = QVersionNumber::fromString(m_imports[k].toString());
        if(ver.isNull()) {
            emit error(toStr("Invalid imports version", m_imports[k].toString(), "for", k));
            return nullptr;
        }
        header += QString("import %1 %2\n").arg(k).arg(m_imports[k].toString());
#else
        header += QString("import %1\n").arg(k);
#endif
    }

/*    if(m_flags & GenerateAccess) {
#ifdef QT5
        header += QString("import FigmaQmlInterface 1.0\n");
#else
        header += QString("import FigmaQmlInterface\n");
#endif
    }
*/
    if((m_flags & GenerateAccess) && !(m_flags & QulMode))
        header += QString("import FigmaQmlInterface\n");

    const auto components = FigmaParser::components(json, *this);

    if(!components) {
        return false;
    }

     TIMED_START(t3)

    /*
    const auto keys = components->keys();
    for (const auto k : keys) {
        qDebug().nospace()
                << k << ';'
                << (*components)[k]->id() << ';'
                << (*components)[k]->key() << ';'
                << (*components)[k]->name();
    }


    static int loopers = 0;
    ++loopers;
    const auto [i, r, n] = mProvider.cacheInfo();
    qDebug() << "loopers" << loopers << i << r << n;
    */

    if(!writeComponents(doc, *components, header)) {
        return false;
    }

    TIMED_END(t3, "Component")
    TIMED_START(t4)


    const auto canvases = FigmaParser::canvases(json, *this);
    if(!canvases)
        return false;

    if(!setDocument(doc, *canvases, *components, header)) {
        return false;
    }

    TIMED_END(t4, "elements")
    return true;
}

#ifdef USE_NATIVE_FONT_DIALOG
void FigmaQml::showFontDialog(const QString& currentFont) {
    auto w = QApplication::activeWindow();
    auto dlg = new QFontDialog(QFont(currentFont), w);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setModal(true);
    QObject::connect(dlg, &QFontDialog::fontSelected, [this](const QFont& font) {
        const auto family = font.family();
        qDebug() << "font selected" << family;
        emit fontAdded(family);
    });
    dlg->show();
}
#endif


void FigmaQml::executeQul(const QVariantMap& parameters, const std::vector<int>& elements) {
#ifdef HAS_QUL
    executeQulApp(parameters, *this, elements);
#else
    (void) parameters;
    (void) elements;
#endif
}

bool FigmaQml::saveCurrentQML(const QVariantMap& parameters, const QString& folderName, bool writeAsApp, const std::vector<int>& elements) {
#ifdef HAS_QUL
    return writeQul(folderName, parameters, *this, writeAsApp, elements);
#else
    (void) parameters;
    (void) elements;
    return true;
#endif
}

bool FigmaQml::hasFontPathInfo() const {
#ifdef Q_OS_LINUX
    return true;
#else
    return false;
#endif
}


void FigmaQml::findFontPath(const QString& fontFamilyName) const {
    const QFont font(fontFamilyName);
    m_fontInfo->getFontFilePath(font);
}

QByteArray FigmaQml::sourceCode(unsigned canvasIndex, unsigned elementIndex) const {
    const auto cit = m_sourceDoc->begin() + canvasIndex;
    const auto eit = (*cit)->begin() + elementIndex;
    return (*eit)->data();
}

