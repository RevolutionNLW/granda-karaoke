#include "AudioOutputs.h"

#include "BackgroundWork.h"
#include "Logging.h"

#include <QHash>
#include <QPointer>
#include <QCoreApplication>
#include <QTimer>

#include <gst/gst.h>

namespace audio {

namespace {

// Every sound output the system reports, each with a reference the caller
// must release with gst_object_unref.
QList<GstDevice*> sinkDevices()
{
    QList<GstDevice*> result;
    GstDeviceMonitor* monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(monitor, "Audio/Sink", nullptr);
    GList* devices = gst_device_monitor_get_devices(monitor);
    for (GList* item = devices; item; item = item->next)
        result.append(GST_DEVICE(item->data));  // the list's reference moves to us
    g_list_free(devices);
    gst_object_unref(monitor);
    return result;
}

QString deviceName(GstDevice* device)
{
    gchar* name = gst_device_get_display_name(device);
    const QString result = QString::fromUtf8(name).trimmed();
    g_free(name);
    return result;
}

} // namespace

// A device's own lasting id, where its system gives one.
QString deviceId(GstDevice* device)
{
    QString id;
    if (GstStructure* properties = gst_device_get_properties(device)) {
        for (const char* key : {"device.id", "unique-id", "device.strid", "wasapi2.device.id",
                                "wasapi.device.id", "device.path", "alsa.device", "object.path"}) {
            if (const gchar* value = gst_structure_get_string(properties, key)) {
                id = QString::fromUtf8(value);
                break;
            }
        }
        gst_structure_free(properties);
    }
    return id;
}

QList<Output> outputs()
{
    QList<Output> result;
    QHash<QString, int> seen;
    for (GstDevice* device : sinkDevices()) {
        const QString name = deviceName(device);
        if (!name.isEmpty()) {
            Output output;
            const int count = ++seen[name];
            output.label = count == 1 ? name : QStringLiteral("%1 (%2)").arg(name).arg(count);
            const QString id = deviceId(device);
            output.id = id.isEmpty() ? output.label : QStringLiteral("id:") + id;
            result.append(output);
        }
        gst_object_unref(device);
    }
    return result;
}

QStringList outputNames()
{
    QStringList names;
    for (const Output& output : outputs())
        names.append(output.label);
    return names;
}

GstElement* createSink(const QString& output)
{
    GstElement* sink = nullptr;
    QHash<QString, int> seen;
    for (GstDevice* device : sinkDevices()) {
        const QString name = deviceName(device);
        const int count = ++seen[name];
        const QString label = count == 1 ? name : QStringLiteral("%1 (%2)").arg(name).arg(count);
        const QString id = deviceId(device);
        const bool match = output.startsWith(QLatin1String("id:"))
            ? !id.isEmpty() && output.mid(3) == id
            : label == output;
        if (!sink && match)
            sink = gst_device_create_element(device, nullptr);
        gst_object_unref(device);
    }
    if (!sink && !output.isEmpty())
        qCWarning(lcPlayer).noquote() << "Sound output not found:" << output;
    return sink;
}

OutputFinder* OutputFinder::instance()
{
    static QPointer<OutputFinder> finder;
    if (!finder)
        finder = new OutputFinder(QCoreApplication::instance());
    return finder;
}

void OutputFinder::forget()
{
    ++m_generation;
    m_outputs.clear();
    m_hasList = false;
}

void OutputFinder::refresh()
{
    if (m_searching) {
        m_again = true;  // one more search when this one ends
        return;
    }
    start();
}

void OutputFinder::start()
{
    m_searching = true;
    m_again = false;
    emit searchingChanged(true);
    const quint64 generation = ++m_generation;
    const Lister lister = m_lister ? m_lister : Lister(&audio::outputs);
    // The worker never touches the finder: its result travels through the
    // application object and is dropped if the finder has gone.
    const QPointer<OutputFinder> self(this);
    background::run(QStringLiteral("AudioOutputs"), [self, lister, generation] {
        const QList<Output> found = lister();
        if (QCoreApplication* app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [self, found, generation] {
                if (!self)
                    return;
                self->m_searching = false;
                if (generation == self->m_generation) {
                    self->m_outputs = found;
                    self->m_hasList = true;
                    emit self->outputsChanged();
                }
                emit self->searchingChanged(false);
                if (self->m_again)
                    self->start();
            }, Qt::QueuedConnection);
        }
    });
}

namespace {
QPointer<TestTone> g_playing;
}

bool TestTone::isPlaying()
{
    return !g_playing.isNull();
}

TestTone* TestTone::play(const QString& outputName, int volumePercent, QString* error)
{
    const auto fail = [error](const QString& message) -> TestTone* {
        if (error)
            *error = message;
        return nullptr;
    };
    if (isPlaying())
        return fail(QStringLiteral("The test sound is already playing."));
    // A soft 660 Hz tone, just under a second, faded by the chosen volume.
    GstElement* pipeline = gst_pipeline_new("test-tone");
    GstElement* source = gst_element_factory_make("audiotestsrc", nullptr);
    GstElement* convert = gst_element_factory_make("audioconvert", nullptr);
    GstElement* volume = gst_element_factory_make("volume", nullptr);
    GstElement* sink = outputName.isEmpty() ? gst_element_factory_make("autoaudiosink", nullptr)
                                            : createSink(outputName);
    if (!pipeline || !source || !convert || !volume || !sink) {
        for (GstElement* element : {pipeline, source, convert, volume, sink}) {
            if (element)
                gst_object_unref(gst_object_ref_sink(element));
        }
        return fail(outputName.isEmpty() || sink
                        ? QStringLiteral("The test sound could not be made.")
                        : QStringLiteral("That sound output is not connected."));
    }
    g_object_set(source, "freq", 660.0, "volume", 0.25, "num-buffers", 40, nullptr);
    g_object_set(volume, "volume", qBound(0, volumePercent, 100) / 100.0, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, convert, volume, sink, nullptr);
    if (!gst_element_link_many(source, convert, volume, sink, nullptr)
        || gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return fail(QStringLiteral("That sound output could not be used."));
    }
    g_playing = new TestTone(pipeline);
    return g_playing;
}

TestTone::TestTone(GstElement* pipeline)
    : m_pipeline(pipeline)
{
    auto* timer = new QTimer(this);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, &TestTone::poll);
    timer->start();
    // However it ends, never longer than a few seconds.
    QTimer::singleShot(4000, this, [this] { deleteLater(); });
}

TestTone::~TestTone()
{
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    emit finished();
}

void TestTone::poll()
{
    GstBus* bus = gst_element_get_bus(m_pipeline);
    while (GstMessage* message = gst_bus_pop_filtered(
               bus, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR))) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
            qCWarning(lcPlayer) << "The test sound could not be played";
        gst_message_unref(message);
        deleteLater();
    }
    gst_object_unref(bus);
}

} // namespace audio
