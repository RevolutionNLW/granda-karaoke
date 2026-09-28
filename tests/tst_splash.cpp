#include "BusTestPlayer.h"
#include "MainWindow.h"
#include "SongSettings.h"
#include "ui/Splash.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

// A window that counts the keys that reach it.
class CountingWidget : public QWidget {
public:
    int keys = 0;
    int wheels = 0;
    int menus = 0;

protected:
    void contextMenuEvent(QContextMenuEvent* event) override
    {
        ++menus;
        event->accept();
    }
    void wheelEvent(QWheelEvent* event) override
    {
        ++wheels;
        event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        ++keys;
        QWidget::keyPressEvent(event);
    }
};

// A stand-in main window: a text box with the keyboard, a button and an
// application-wide shortcut.
struct Window {
    std::unique_ptr<CountingWidget> widget = std::make_unique<CountingWidget>();
    QLineEdit* search = new QLineEdit(widget.get());
    QPushButton* button = new QPushButton(QStringLiteral("Sing"), widget.get());
    QAction* action = new QAction(widget.get());

    Window()
    {
        auto* layout = new QVBoxLayout(widget.get());
        layout->addWidget(search);
        layout->addWidget(button);
        action->setShortcut(QKeySequence(Qt::Key_F7));
        action->setShortcutContext(Qt::ApplicationShortcut);
        widget->addAction(action);
        widget->resize(800, 500);
        widget->show();
        search->setFocus();
        QCoreApplication::processEvents();
    }
};

// Lets deleteLater() happen.
void settle()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

// How much of the picture is the gold accent (the mark and the waveform).
int goldPixels(const QImage& image)
{
    int count = 0;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 150 && pixel.green() > 90 && pixel.blue() < 110 && pixel.red() - pixel.blue() > 90)
                ++count;
        }
    }
    return count;
}

} // namespace

class TestSplash : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void coversTheWindowThenRemovesItself();
    void neverOutlastsTheLaunchLimit();
    void anyKeySkipsItAndIsUsedUp();
    void shortcutsDoNotActWhileItIsUp();
    void clicksSkipItWithoutReachingTheWindow();
    void keysAimedAtTheWindowItselfAreUsedUpToo();
    void heldKeysAndDoubleClicksAreUsedUpWhole();
    void aLongScrollIsUsedUpWhole();
    void newInputOfTheOtherKindGoesThrough();
    void keysWithoutACodeAreUsedUpWhole();
    void noMenuOpensUnderIt();
    void followsTheWindowSize();
    void goesWithItsWindow();
    void closingTheProgramAroundTheSplashIsSafe_data();
    void closingTheProgramAroundTheSplashIsSafe();
    void drawsTheNameAndMarkAtEveryScale();
};

void TestSplash::initTestCase()
{
    theme::apply(*qobject_cast<QApplication*>(QCoreApplication::instance()));
}

void TestSplash::coversTheWindowThenRemovesItself()
{
    Window window;
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    QSignalSpy finished(splash.data(), &ui::SplashOverlay::finished);
    QVERIFY(splash->isVisible());
    QCOMPARE(splash->geometry(), window.widget->rect());
    QCOMPARE(window.widget->children().last(), splash.data());  // on top
    QCOMPARE(splash->plannedMs(), ui::SplashOverlay::kNormalMs);
    // The window keeps the keyboard; the splash never takes it.
    QCOMPARE(window.widget->focusWidget(), window.search);

    QElapsedTimer timer;
    timer.start();
    // The name is up by about 1.2 s and stays until the dissolve at 2.7 s.
    QVERIFY(splash->titleShownMs() >= 1000 && splash->titleShownMs() <= 1300);
    QVERIFY(splash->dissolveStartMs() >= 2600 && splash->dissolveStartMs() <= 2800);
    QTest::qWait(splash->dissolveStartMs() - 200);
    QVERIFY(splash && splash->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(splash.isNull(), ui::SplashOverlay::kLongestMs + 500);
    QCOMPARE(finished.count(), 1);
    QVERIFY(timer.elapsed() >= ui::SplashOverlay::kNormalMs - 50);
    QVERIFY(timer.elapsed() < ui::SplashOverlay::kLongestMs + 500);
}

void TestSplash::neverOutlastsTheLaunchLimit()
{
    Window window;
    const auto planned = [&window](qint64 sinceLaunch, int length = ui::SplashOverlay::kNormalMs) {
        auto* splash = new ui::SplashOverlay(window.widget.get(), sinceLaunch, length);
        const int ms = splash->plannedMs();
        splash->dismiss();
        settle();
        return ms;
    };
    // A normal start (under a second here) gets the whole 3.5 s.
    QCOMPARE(planned(0), ui::SplashOverlay::kNormalMs);
    QCOMPARE(planned(900), ui::SplashOverlay::kNormalMs);
    // A slower start gets a shorter splash: over by 4.5 s from launch...
    QCOMPARE(planned(1500), ui::SplashOverlay::kLatestFromLaunchMs - 1500);
    QVERIFY(1500 + planned(1500) <= ui::SplashOverlay::kLatestFromLaunchMs);
    // ...except for a brief look when the start itself took that long.
    QCOMPARE(planned(3500), ui::SplashOverlay::kShortestMs);
    QCOMPARE(planned(10000), ui::SplashOverlay::kShortestMs);
    // Its own length never goes past 4 s.
    QCOMPARE(planned(0, 3000), 3000);
    QCOMPARE(planned(0, 60000), ui::SplashOverlay::kLongestMs);
    QCOMPARE(planned(0, 10), ui::SplashOverlay::kShortestMs);
    QVERIFY(ui::SplashOverlay::kLongestMs <= 4000);
}

void TestSplash::anyKeySkipsItAndIsUsedUp()
{
    Window window;
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyClick(window.search, Qt::Key_A);
    settle();
    QVERIFY(splash.isNull());
    // The key only skipped the splash; it did not type.
    QVERIFY(window.search->text().isEmpty());
    // Afterwards keys work as normal.
    QTest::keyClick(window.search, Qt::Key_B);
    QCOMPARE(window.search->text(), QStringLiteral("b"));

    // Enter and Escape skip it too, and do nothing else.
    for (const Qt::Key key : {Qt::Key_Return, Qt::Key_Escape, Qt::Key_Space}) {
        splash = new ui::SplashOverlay(window.widget.get());
        QSignalSpy returned(window.search, &QLineEdit::returnPressed);
        QTest::keyClick(window.search, key);
        settle();
        QVERIFY(splash.isNull());
        QCOMPARE(returned.count(), 0);
        QCOMPARE(window.search->text(), QStringLiteral("b"));
    }
}

void TestSplash::shortcutsDoNotActWhileItIsUp()
{
    Window window;
    QSignalSpy triggered(window.action, &QAction::triggered);
    // The test's own check: the shortcut works without a splash.
    QTest::keyClick(window.search, Qt::Key_F7);
    QCOMPARE(triggered.count(), 1);

    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyClick(window.search, Qt::Key_F7);
    settle();
    QVERIFY(splash.isNull());
    QCOMPARE(triggered.count(), 1);  // only skipped the splash

    QTest::keyClick(window.search, Qt::Key_F7);
    QCOMPARE(triggered.count(), 2);
}

void TestSplash::clicksSkipItWithoutReachingTheWindow()
{
    Window window;
    QSignalSpy clicked(window.button, &QPushButton::clicked);
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    // A click where the button is lands on the splash, which covers it.
    const QPoint onButton = window.button->geometry().center();
    QCOMPARE(window.widget->childAt(onButton), splash.data());
    QTest::mouseClick(splash, Qt::LeftButton, Qt::NoModifier, onButton);
    QVERIFY(!splash->isVisible());  // out of sight at once
    QTRY_VERIFY(splash.isNull());
    QCOMPARE(clicked.count(), 0);

    // Even a click delivered to the button itself only skips the splash.
    splash = new ui::SplashOverlay(window.widget.get());
    QTest::mouseClick(window.button, Qt::LeftButton);
    QTRY_VERIFY(splash.isNull());
    QCOMPARE(clicked.count(), 0);

    QTest::mouseClick(window.button, Qt::LeftButton);
    QCOMPARE(clicked.count(), 1);
}

void TestSplash::keysAimedAtTheWindowItselfAreUsedUpToo()
{
    // Before a music folder is chosen the window itself has the keyboard.
    Window window;
    window.widget->setFocusPolicy(Qt::StrongFocus);
    window.widget->setFocus();
    QSignalSpy triggered(window.action, &QAction::triggered);
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyClick(window.widget.get(), Qt::Key_A);
    settle();
    QVERIFY(splash.isNull());
    QCOMPARE(window.widget->keys, 0);

    splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyClick(window.widget.get(), Qt::Key_F7);
    settle();
    QVERIFY(splash.isNull());
    QCOMPARE(triggered.count(), 0);
    QCOMPARE(window.widget->keys, 0);

    QTest::keyClick(window.widget.get(), Qt::Key_A);
    QCOMPARE(window.widget->keys, 1);
}

void TestSplash::heldKeysAndDoubleClicksAreUsedUpWhole()
{
    Window window;
    // A held key: its repeats and its release are used up with it.
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyPress(window.search, Qt::Key_A);
    QVERIFY(!splash->isVisible());
    for (int i = 0; i < 3; ++i) {
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"), true);
        QApplication::sendEvent(window.search, &repeat);
    }
    QVERIFY(window.search->text().isEmpty());
    QTest::keyRelease(window.search, Qt::Key_A);
    settle();
    QVERIFY(splash.isNull());
    QTest::keyClick(window.search, Qt::Key_B);
    QCOMPARE(window.search->text(), QStringLiteral("b"));

    // A held shortcut does not act while its key stays down.
    QSignalSpy triggered(window.action, &QAction::triggered);
    splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyPress(window.search, Qt::Key_F7);
    QTest::keyPress(window.search, Qt::Key_F7);
    QTest::keyPress(window.search, Qt::Key_F7);
    QCOMPARE(triggered.count(), 0);
    QTest::keyRelease(window.search, Qt::Key_F7);
    settle();
    QVERIFY(splash.isNull());
    QTest::keyClick(window.search, Qt::Key_F7);
    QCOMPARE(triggered.count(), 1);

    // A double-click: neither click reaches the button beneath.
    QSignalSpy clicked(window.button, &QPushButton::clicked);
    splash = new ui::SplashOverlay(window.widget.get());
    QTest::mouseClick(window.button, Qt::LeftButton);
    QTest::qWait(qMin(100, QApplication::doubleClickInterval() / 3));
    // The second click of the double-click, a moment later.
    QTest::mouseClick(window.button, Qt::LeftButton);
    QTest::mouseDClick(window.button, Qt::LeftButton);
    QTRY_VERIFY(splash.isNull());
    QCOMPARE(clicked.count(), 0);
    QTest::mouseClick(window.button, Qt::LeftButton);
    QCOMPARE(clicked.count(), 1);

    // A release that never comes does not keep the window deaf for long.
    splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyPress(window.search, Qt::Key_C);
    QTRY_VERIFY_WITH_TIMEOUT(splash.isNull(), ui::SplashOverlay::kLongestDrainMs + 500);
    QTest::keyClick(window.search, Qt::Key_D);
    QCOMPARE(window.search->text(), QStringLiteral("bd"));
}

namespace {

void wheel(QWidget* target, Qt::ScrollPhase phase = Qt::NoScrollPhase)
{
    const QPointF at(target->width() / 2.0, target->height() / 2.0);
    QWheelEvent event(at, target->mapToGlobal(at), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                      phase, false);
    QApplication::sendEvent(target, &event);
}

} // namespace

void TestSplash::aLongScrollIsUsedUpWhole()
{
    Window window;
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    // A scroll that goes on for longer than a double-click's time.
    const int step = qMax(40, QApplication::doubleClickInterval() / 3);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < QApplication::doubleClickInterval() * 2) {
        wheel(window.widget.get());
        QTest::qWait(step);
        QVERIFY(splash);  // still using it up
    }
    QCOMPARE(window.widget->wheels, 0);
    QTRY_VERIFY(splash.isNull());
    wheel(window.widget.get());
    QCOMPARE(window.widget->wheels, 1);
}

void TestSplash::newInputOfTheOtherKindGoesThrough()
{
    Window window;
    // A click skips it; typing straight afterwards is not lost.
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    QTest::mouseClick(splash, Qt::LeftButton, Qt::NoModifier, window.button->geometry().center());
    QVERIFY(splash);
    QTest::keyClick(window.search, Qt::Key_X);
    settle();
    QVERIFY(splash.isNull());
    QCOMPARE(window.search->text(), QStringLiteral("x"));

    // A key skips it; a click while that key is still down is not lost.
    QSignalSpy clicked(window.button, &QPushButton::clicked);
    splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyPress(window.search, Qt::Key_Shift);
    QTest::mouseClick(window.button, Qt::LeftButton);
    settle();
    QVERIFY(splash.isNull());
    QCOMPARE(clicked.count(), 1);
    QTest::keyRelease(window.search, Qt::Key_Shift);

    // But the keys of one chord (a modifier, then its letter) all belong to
    // the press that skipped it.
    QSignalSpy triggered(window.action, &QAction::triggered);
    window.action->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_K));
    splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyPress(window.search, Qt::Key_Control);
    QTest::keyClick(window.search, Qt::Key_K, Qt::ControlModifier);
    QCOMPARE(triggered.count(), 0);
    QTest::keyRelease(window.search, Qt::Key_Control);
    settle();
    QVERIFY(splash.isNull());
    QTest::keyClick(window.search, Qt::Key_K, Qt::ControlModifier);
    QCOMPARE(triggered.count(), 1);
}

void TestSplash::keysWithoutACodeAreUsedUpWhole()
{
    // Some typed characters (e.g. from a compose sequence) come without a key code.
    Window window;
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    const auto send = [&window](QEvent::Type type, bool repeat) {
        QKeyEvent event(type, 0, Qt::NoModifier, QStringLiteral("é"), repeat);
        QApplication::sendEvent(window.search, &event);
    };
    send(QEvent::KeyPress, false);
    QVERIFY(splash);
    QVERIFY(!splash->isVisible());
    send(QEvent::KeyPress, true);
    send(QEvent::KeyPress, true);
    QVERIFY(window.search->text().isEmpty());
    send(QEvent::KeyRelease, false);
    settle();
    QVERIFY(splash.isNull());
    QVERIFY(window.search->text().isEmpty());
}

void TestSplash::noMenuOpensUnderIt()
{
    Window window;
    const auto askForMenu = [&window](QContextMenuEvent::Reason reason) {
        const QPoint at(10, 10);
        QContextMenuEvent event(reason, at, window.widget->mapToGlobal(at));
        QApplication::sendEvent(window.widget.get(), &event);
    };
    // A right-click (Windows asks for the menu on release).
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    QTest::mousePress(window.widget.get(), Qt::RightButton, Qt::NoModifier, QPoint(10, 10));
    askForMenu(QContextMenuEvent::Mouse);
    QTest::mouseRelease(window.widget.get(), Qt::RightButton, Qt::NoModifier, QPoint(10, 10));
    askForMenu(QContextMenuEvent::Mouse);
    QTRY_VERIFY(splash.isNull());
    QCOMPARE(window.widget->menus, 0);
    // The Menu key.
    splash = new ui::SplashOverlay(window.widget.get());
    QTest::keyPress(window.search, Qt::Key_Menu);
    askForMenu(QContextMenuEvent::Keyboard);
    QTest::keyRelease(window.search, Qt::Key_Menu);
    settle();
    QVERIFY(splash.isNull());
    QCOMPARE(window.widget->menus, 0);
    // Afterwards menus open as normal.
    askForMenu(QContextMenuEvent::Mouse);
    QCOMPARE(window.widget->menus, 1);
}

void TestSplash::followsTheWindowSize()
{
    Window window;
    QPointer<ui::SplashOverlay> splash = new ui::SplashOverlay(window.widget.get());
    window.widget->resize(1366, 768);
    QCoreApplication::processEvents();
    QCOMPARE(splash->geometry(), window.widget->rect());
    // A widget added to the window later still stays under it.
    auto* late = new QPushButton(QStringLiteral("Late"), window.widget.get());
    late->show();
    QCoreApplication::processEvents();
    QCOMPARE(window.widget->children().last(), splash.data());
    splash->dismiss();
    settle();
}

void TestSplash::goesWithItsWindow()
{
    QPointer<ui::SplashOverlay> splash;
    {
        Window window;
        splash = new ui::SplashOverlay(window.widget.get());
        QTest::qWait(100);
    }
    QVERIFY(splash.isNull());
    // Nothing is left listening to the keyboard.
    QLineEdit other;
    other.show();
    other.setFocus();
    QTest::keyClick(&other, Qt::Key_X);
    QCOMPARE(other.text(), QStringLiteral("x"));
    QTest::qWait(ui::SplashOverlay::kNormalMs);
}

void TestSplash::drawsTheNameAndMarkAtEveryScale()
{
    for (const int percent : {80, 100, 125, 150}) {
        theme::setScalePercent(percent);
        for (const QSize size : {QSize(1366, 768), QSize(640, 400), QSize(1920, 1080)}) {
            Window window;
            window.widget->resize(size);
            QPointer<ui::SplashOverlay> splash
                = new ui::SplashOverlay(window.widget.get(), 0, ui::SplashOverlay::kShortestMs);
            QTest::qWait(splash->titleShownMs() + 100);
            QVERIFY(splash);
            const QImage picture = splash->grab().toImage();
            QVERIFY2(goldPixels(picture) > 20, qPrintable(QStringLiteral("%1% %2x%3").arg(percent)
                                                               .arg(size.width()).arg(size.height())));
            // The corners are the plain dark background.
            const QColor corner = picture.pixelColor(2, 2);
            QVERIFY(corner.lightness() < 40);
            splash->dismiss();
            settle();
        }
    }
    theme::setScalePercent(100);
}

void TestSplash::closingTheProgramAroundTheSplashIsSafe_data()
{
    QTest::addColumn<int>("closeAtMs");
    QTest::addColumn<bool>("skipFirst");
    QTest::newRow("at once") << 0 << false;
    QTest::newRow("mid-way") << 1500 << false;
    QTest::newRow("just after it ends") << ui::SplashOverlay::kLongestMs + 100 << false;
    QTest::newRow("skipped, then closed quickly") << 150 << true;
}

void TestSplash::closingTheProgramAroundTheSplashIsSafe()
{
    // The real main window with its splash, closed and destroyed as the
    // program does, at different moments. Afterwards nothing of the splash
    // is left: no event filter on the application, no timer.
    QFETCH(int, closeAtMs);
    QFETCH(bool, skipFirst);
    QTemporaryDir temporary;
    QPointer<ui::SplashOverlay> splash;
    {
        BusTestPlayer player;
        SongSettingsStore settings(temporary.filePath(QStringLiteral("settings.json")));
        MainWindow window(&player, &settings);
        splash = new ui::SplashOverlay(&window);
        window.resize(900, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        if (skipFirst) {
            QTest::keyClick(window.windowHandle(), Qt::Key_Space);
            QTRY_VERIFY(splash.isNull());
        }
        QTest::qWait(closeAtMs);
        window.close();
    }
    settle();
    QVERIFY(splash.isNull());
    QLineEdit other;
    other.show();
    other.setFocus();
    QTest::keyClick(&other, Qt::Key_X);
    QCOMPARE(other.text(), QStringLiteral("x"));
    QTest::qWait(300);  // any timer it left behind would fire here
    QCOMPARE(other.text(), QStringLiteral("x"));
}

QTEST_MAIN(TestSplash)
#include "tst_splash.moc"
