#include <QtTest>

#include "AppConfig.h"
#include "LineIoManager.h"
#define private public
#include "TestController.h"
#undef private
#include "linesmodel.h"
#include "modbusbus.h"

class TestControllerTests : public QObject
{
    Q_OBJECT

private:
    static void addTestLines(LinesModel &lines, int count)
    {
        for (int i = 0; i < count; ++i) {
            lines.addLine(QStringLiteral("Line %1").arg(i + 1),
                          100.0, 100.0, 1.0, 100.0, 5.0,
                          Line::Constant, Line::OK, Line::On);
        }
    }

private slots:
    void allLinesWarmUpWithoutMeterConnection()
    {
        AppConfig config;
        ModbusBus bus(&config);
        bus.setRelayModuleCount(2);
        LinesModel lines;
        addTestLines(lines, 2);
        LineIoManager io;
        io.bind(&bus, &lines, 2);
        TestController controller;
        controller.setModel(&lines);
        controller.setIoManager(&io);
        controller.setBus(&bus);

        controller.startTestOperator(-2, 60);

        QVERIFY(io.noMeasTestActive());
        QVERIFY(!io.stepTestActive());
        QCOMPARE(controller.measuredLine(), -1);

        controller.stopTest(-1);
    }

    void calculatedDurationIncludesWarmupMeasurementsAndRelayGaps()
    {
        LinesModel lines;
        addTestLines(lines, 2);
        TestController controller;
        controller.setModel(&lines);

        QCOMPARE(controller.calcAllLinesTestDurationSec(), 65);
    }

    void firstMeasurementStartsOnlyAfterWarmupPhase()
    {
        AppConfig config;
        ModbusBus bus(&config);
        bus.setRelayModuleCount(1);
        LinesModel lines;
        addTestLines(lines, 2);
        LineIoManager io;
        io.bind(&bus, &lines, 2);
        TestController controller;
        controller.setModel(&lines);
        controller.setIoManager(&io);
        controller.setBus(&bus);
        controller.m_shotTimeWarm = 0;
        controller.m_meterSettlingMs = 0;

        controller.startAllLinesTest(2600);

        QVERIFY(io.noMeasTestActive());
        QVERIFY(!io.stepTestActive());
        QCOMPARE(controller.measuredLine(), -1);
        QTRY_VERIFY_WITH_TIMEOUT(io.stepTestActive(), 200);
        QCOMPARE(controller.measuredLine(), -1);
        emit io.measurementConnected(0);
        QCOMPARE(controller.measuredLine(), -1);
        QVERIFY(controller.m_allTestPhase == TestController::AllTestPhase::Settling);

        controller.stopTest(-1);
    }

    void missingRelayAcknowledgementAbortsAllLinesTest()
    {
        AppConfig config;
        ModbusBus bus(&config);
        LinesModel lines;
        addTestLines(lines, 2);
        LineIoManager io;
        io.bind(&bus, &lines, 2);
        TestController controller;
        controller.setModel(&lines);
        controller.setIoManager(&io);
        controller.setBus(&bus);
        controller.startTestOperator(-2, 120);
        controller.startCurrentAllLinesMeasurement();
        QCOMPARE(controller.measuredLine(), -1);
        QVERIFY(controller.testActive());
        QVERIFY(QMetaObject::invokeMethod(controller.m_allTestTimer, "timeout", Qt::DirectConnection));
        QVERIFY(!controller.testActive());
        QVERIFY(!io.stepTestActive());
        QCOMPARE(controller.measuredLine(), -1);
    }

    void singleLineIgnoresSamplesUntilRelayAcknowledgement()
    {
        AppConfig config;
        ModbusBus bus(&config);
        LinesModel lines;
        addTestLines(lines, 1);
        LineIoManager io;
        io.bind(&bus, &lines, 1);
        TestController controller;
        controller.setModel(&lines);
        controller.setIoManager(&io);
        controller.setBus(&bus);
        controller.startTestOperator(0, 60);
        QCOMPARE(controller.measuredLine(), -1);
        emit io.measurementConnected(0);
        QCOMPARE(controller.measuredLine(), 0);
        QCOMPARE(controller.m_singleTestTimer->interval(), 60000);
        controller.stopTest(-1);
    }

    void scanStartsBeforeRequestedEndWithBudgetForEveryLine()
    {
        AppConfig config;
        ModbusBus bus(&config);
        LinesModel lines;
        addTestLines(lines, 2);
        LineIoManager io;
        io.bind(&bus, &lines, 2);
        TestController controller;
        controller.setModel(&lines);
        controller.setIoManager(&io);
        controller.setBus(&bus);
        controller.startTestOperator(-2, 120);
        QCOMPARE(controller.plannedTestDurationSec(), 120);
        QCOMPARE(controller.m_allTestTimer->interval(), 115400);
        controller.stopTest(-1);
    }

    void onlyRequestedSampleAfterSettlingCanFinishLine()
    {
        AppConfig config;
        ModbusBus bus(&config);
        LinesModel lines;
        addTestLines(lines, 2);
        LineIoManager io;
        io.bind(&bus, &lines, 2);
        TestController controller;
        controller.setModel(&lines);
        controller.setIoManager(&io);
        controller.setBus(&bus);
        controller.startTestOperator(-2, 120);
        controller.startCurrentAllLinesMeasurement();
        emit io.measurementConnected(0);
        QCOMPARE(controller.m_allTestTimer->interval(), 1000);
        emit bus.testMeterSampleReady(1, 100.0, 1.0, 100.0);
        QCOMPARE(lines.line(0)->status(), Line::Test);
        QVERIFY(QMetaObject::invokeMethod(controller.m_allTestTimer, "timeout", Qt::DirectConnection));
        emit bus.testMeterSampleReady(controller.m_meterSampleSequence - 1, 100.0, 1.0, 100.0);
        QCOMPARE(lines.line(0)->status(), Line::Test);
        emit bus.testMeterSampleReady(controller.m_meterSampleSequence, 100.0, 1.0, 100.0);
        QCOMPARE(lines.line(0)->status(), Line::OK);
        QCOMPARE(controller.m_allTestCurrentIndex, 1);
        QCOMPARE(controller.measuredLine(), -1);
        controller.stopTest(-1);
    }

    void completedScanKeepsCabinetInTestUntilRequestedEnd()
    {
        AppConfig config;
        ModbusBus bus(&config);
        LinesModel lines;
        addTestLines(lines, 1);
        LineIoManager io;
        io.bind(&bus, &lines, 1);
        TestController controller;
        controller.setModel(&lines);
        controller.setIoManager(&io);
        controller.setBus(&bus);
        controller.startTestOperator(-2, 120);
        controller.startCurrentAllLinesMeasurement();
        emit io.measurementConnected(0);
        QVERIFY(QMetaObject::invokeMethod(controller.m_allTestTimer, "timeout", Qt::DirectConnection));
        emit bus.testMeterSampleReady(controller.m_meterSampleSequence, 100.0, 1.0, 100.0);
        QVERIFY(controller.testActive());
        QVERIFY(io.noMeasTestActive());
        QVERIFY(controller.m_allTestPhase == TestController::AllTestPhase::Holding);
        controller.stopTest(-1);
    }

    void missingMeterSampleCannotProducePassedResult()
    {
        AppConfig config;
        ModbusBus bus(&config);
        LinesModel lines;
        addTestLines(lines, 1);
        LineIoManager io;
        io.bind(&bus, &lines, 1);
        TestController controller;
        controller.setModel(&lines);
        controller.setIoManager(&io);
        controller.setBus(&bus);
        controller.startTestOperator(-2, 120);
        controller.startCurrentAllLinesMeasurement();
        emit io.measurementConnected(0);
        QVERIFY(QMetaObject::invokeMethod(controller.m_allTestTimer, "timeout", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(controller.m_allTestTimer, "timeout", Qt::DirectConnection));
        QVERIFY(!controller.testActive());
        QVERIFY(!lines.line(0)->lastMeasuredTest().isValid());
    }
};

QTEST_MAIN(TestControllerTests)
#include "test_controller_tests.moc"
