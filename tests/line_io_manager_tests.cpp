#include <QObject>
#include <QTimer>
#include <QDateTime>
#include <QtGlobal>
#include <QtTest>

#define private public
#include "LineIoManager.h"
#undef private

#include "AppConfig.h"
#include "linesmodel.h"
#include "modbusbus.h"

class LineIoManagerTests : public QObject
{
    Q_OBJECT

private:
    static bool relayOn(const LineIoManager &io, int module, int key)
    {
        return (io.m_desiredRelays[module] & (quint8(1u) << (key - 1))) != 0;
    }

    static void addTestLines(LinesModel &lines, int count)
    {
        for (int i = 0; i < count; ++i) {
            lines.addLine(QStringLiteral("Line %1").arg(i + 1),
                          100.0, 100.0, 1.0, 100.0, 5.0,
                          Line::Constant, Line::OK, Line::On);
        }
    }

private slots:
    void measuredLineKeepsWorkingPathEnabled()
    {
        AppConfig config;
        ModbusBus bus(&config);
        bus.setRelayModuleCount(2);
        LinesModel lines;
        addTestLines(lines, 5);
        LineIoManager io;
        io.m_twoStepDelayMs = 10;
        io.bind(&bus, &lines, 5);

        QVERIFY(io.requestStepTestStart(0));
        QTRY_VERIFY_WITH_TIMEOUT(relayOn(io, 0, 4), 200);
        QTest::qWait(50);

        QVERIFY(relayOn(io, 0, 1));
        QVERIFY(relayOn(io, 0, 4));
        QVERIFY(!relayOn(io, 0, 3));
    }

    void switchingLinesBreaksBeforeMaking()
    {
        AppConfig config;
        ModbusBus bus(&config);
        bus.setRelayModuleCount(2);
        LinesModel lines;
        addTestLines(lines, 5);
        LineIoManager io;
        io.m_twoStepDelayMs = 20;
        io.bind(&bus, &lines, 5);

        QVERIFY(io.requestStepTestStart(0));
        QTRY_VERIFY_WITH_TIMEOUT(relayOn(io, 0, 4), 200);

        QVERIFY(io.requestStepTestStart(1));
        QVERIFY(!relayOn(io, 0, 4));
        QVERIFY(!relayOn(io, 0, 6));

        QTRY_VERIFY_WITH_TIMEOUT(relayOn(io, 0, 6), 200);
        QVERIFY(!relayOn(io, 0, 4));
    }

    void switchingAcrossModulesKeepsOnlyOneMeasurementRelayOn()
    {
        AppConfig config;
        ModbusBus bus(&config);
        bus.setRelayModuleCount(2);
        LinesModel lines;
        addTestLines(lines, 5);
        LineIoManager io;
        io.m_twoStepDelayMs = 20;
        io.bind(&bus, &lines, 5);

        QVERIFY(io.requestStepTestStart(2));
        QTRY_VERIFY_WITH_TIMEOUT(relayOn(io, 0, 8), 200);

        QVERIFY(io.requestStepTestStart(3));
        QVERIFY(!relayOn(io, 0, 8));
        QVERIFY(!relayOn(io, 1, 2));

        QTRY_VERIFY_WITH_TIMEOUT(relayOn(io, 1, 2), 200);
        QVERIFY(!relayOn(io, 0, 4));
        QVERIFY(!relayOn(io, 0, 6));
        QVERIFY(!relayOn(io, 0, 8));
        QVERIFY(!relayOn(io, 1, 4));
    }

    void stoppingDuringGapPreventsDelayedMeasurementConnection()
    {
        AppConfig config;
        ModbusBus bus(&config);
        bus.setRelayModuleCount(1);
        LinesModel lines;
        addTestLines(lines, 3);
        LineIoManager io;
        io.m_twoStepDelayMs = 30;
        io.bind(&bus, &lines, 3);

        QVERIFY(io.requestStepTestStart(0));
        io.requestStepTestStop();
        QTest::qWait(60);

        QVERIFY(!io.stepTestActive());
        QVERIFY(!relayOn(io, 0, 4));
        QVERIFY(!relayOn(io, 0, 6));
        QVERIFY(!relayOn(io, 0, 8));
    }

    void singleLineTestKeepsAllWorkingPathsEnabled()
    {
        AppConfig config;
        ModbusBus bus(&config);
        bus.setRelayModuleCount(2);
        LinesModel lines;
        addTestLines(lines, 5);
        LineIoManager io;
        io.bind(&bus, &lines, 5);

        QVERIFY(io.requestSingleLineTestStart(0));

        QVERIFY(relayOn(io, 0, 1));
        QTRY_VERIFY_WITH_TIMEOUT(relayOn(io, 0, 4), 500);
        QVERIFY(!relayOn(io, 0, 3));
        QVERIFY(!relayOn(io, 0, 5));
        QVERIFY(!relayOn(io, 0, 7));
        QVERIFY(!relayOn(io, 1, 1));
        QVERIFY(!relayOn(io, 1, 3));
    }

};

QTEST_MAIN(LineIoManagerTests)
#include "line_io_manager_tests.moc"
