#include "TestController.h"
#include "modbusbus.h"
#include "logger.h"

static Line::Status safeRestoreStatus(Line::Status st)
{
    if (st == Line::Test)
        return Line::Failure;   // лучше заменить на Line::Unknown / Line::Idle, если такой статус есть
    return st;
}

int TestController::remainingTestDurationSec() const
{
    if (!testActive() || !m_testEndsAt.isValid())
        return 0;

    return qMax(0, int(QDateTime::currentDateTime().secsTo(m_testEndsAt)));
}

void TestController::setCurrentTestKind(TestKind k)
{
    if (m_currentTestKind == k)
        return;

    m_currentTestKind = k;
    emit currentTestKindChanged();
}

void TestController::setTestTiming(int plannedDurationSec)
{
    const int normalized = qMax(0, plannedDurationSec);
    const QDateTime newEndsAt = normalized > 0
        ? QDateTime::currentDateTime().addSecs(normalized)
        : QDateTime();

    if (m_plannedTestDurationSec == normalized && m_testEndsAt == newEndsAt)
        return;

    m_plannedTestDurationSec = normalized;
    m_testEndsAt = newEndsAt;
    emit testTimingChanged();
}

void TestController::clearTestTiming()
{
    if (m_plannedTestDurationSec == 0 && !m_testEndsAt.isValid())
        return;

    m_plannedTestDurationSec = 0;
    m_testEndsAt = QDateTime();
    emit testTimingChanged();
}

TestController::TestController(QObject *parent)
    : QObject(parent)
    , m_allTestTimer(new QTimer(this))
{
    m_allTestTimer->setSingleShot(true);
    m_allTestTimer->setTimerType(Qt::PreciseTimer);

    connect(m_allTestTimer, &QTimer::timeout, this, [this]() {
        if (m_allTestPhase == AllTestPhase::Warmup)
            startCurrentAllLinesMeasurement();
        else if (m_allTestPhase == AllTestPhase::Settling)
            requestCurrentAllLinesSample();
        else if (m_allTestPhase == AllTestPhase::Holding)
            finishAllLinesTest();
        else if (m_allTestPhase == AllTestPhase::Connecting
                 || m_allTestPhase == AllTestPhase::Reading) {
            log("Test relay/meter response timeout; test aborted");
            stopAnyActiveTest();
        }
    });

    loadMaintenance();
}

void TestController::setIoManager(LineIoManager *io)
{
    if (m_io)
        disconnect(m_io, &LineIoManager::measurementConnected, this, nullptr);
    m_io = io;
    if (!m_io)
        return;
    connect(m_io, &LineIoManager::measurementConnected, this, [this](int index) {
        if (m_allTestRunning && m_allTestPhase == AllTestPhase::Connecting
            && index == m_allTestCurrentIndex) {
            m_allTestPhase = AllTestPhase::Settling;
            m_allTestTimer->start(m_meterSettlingMs);
        } else if (m_active == Active::Single && index == m_singleTestIndex) {
            setMeasuredLine(index);
            if (m_singleTestTimer)
                m_singleTestTimer->start(m_singleTestDurationMs);
        }
    });
}

void TestController::setBus(ModbusBus *bus)
{
    if (m_bus)
        disconnect(m_bus, &ModbusBus::testMeterSampleReady, this, &TestController::onTestMeterSample);
    m_bus = bus;
    if (m_bus)
        connect(m_bus, &ModbusBus::testMeterSampleReady, this, &TestController::onTestMeterSample);
}

// ===== API =====

void TestController::startTestOperator(int lineIndex, int sec)
{
    stopAnyActiveTest();
    setSource(Source::Operator);
    startTestInternal(lineIndex, sec);
}

bool TestController::startTestScheduleAll(int sec)
{
    if (!m_model || !m_io)
        return false;

    if (sec < minSecTestDuration)
        sec = minSecTestDuration;

    if (testActive() && m_source == Source::Operator) {
        log("⛔ Schedule: запуск теста всех линий запрещён — идёт операторский тест");
        return false;
    }

    if (testActive() && m_source == Source::Schedule && m_currentAllIsLong) {
        log("⛔ Schedule: запуск запрещён — уже идёт длинный тест по расписанию");
        return false;
    }

    stopAnyActiveTest();
    setSource(Source::Schedule);

    startTestInternal(-2, sec);
    return true;
}

void TestController::startTest(int lineIndex, int sec)
{
    startTestOperator(lineIndex, sec);
}

void TestController::startTestNoMeasure(int sec)
{
    if (!m_io)
        return;
    if (sec <= 0)
        return;

    stopAnyActiveTest();

    log(QString("Тест без измерений на %1 секунд").arg(sec));

    setActive(Active::NoMeas);
    setCurrentTestKind(TestKind::Functional);

    if (!m_io->requestNoMeasTestStart()) {
        setActive(Active::None);
        setSource(Source::None);
        setCurrentTestKind(TestKind::None);
        clearTestTiming();
        return;
    }

    setTestTiming(sec);

    m_noMeasTimer = new QTimer(this);
    m_noMeasTimer->setSingleShot(true);

    connect(m_noMeasTimer, &QTimer::timeout, this, [this]() {
        if (m_io)
            m_io->requestNoMeasTestStop();

        log("Тест без измерений завершён");
        cleanupNoMeasTimer();
        setActive(Active::None);
        setSource(Source::None);
        setCurrentTestKind(TestKind::None);
        clearTestTiming();
    });

    m_noMeasTimer->start(sec * 1000);
}

void TestController::stopTest(int lineIndex)
{
    Q_UNUSED(lineIndex);
    stopAnyActiveTest();
    log("отправлена команда Стоп Тест");
}

// ===== internal =====

void TestController::startTestInternal(int lineIndex, int sec)
{
    if (!m_model || !m_io)
        return;

    const int fullSec = calcAllLinesTestDurationSec();

    if (lineIndex == -1) {
        sec = fullSec;
    } else if (lineIndex > -1) {
        if (sec < minSecTestDuration)
            sec = minSecTestDuration;
    } else if (sec < fullSec) {
        sec = fullSec;
    }

    if (lineIndex < -2 || lineIndex >= m_model->rowCount())
        return;

    const int durationMs = sec * 1000;

    beginMeasurements();

    if (lineIndex >= 0)
        startSingleLineTest(lineIndex, durationMs);
    else
        startAllLinesTest(durationMs);

    setCurrentTestKind((sec >= m_longThresholdMin * 60) ? TestKind::Duration
                                                        : TestKind::Functional);
}

void TestController::stopAnyActiveTest()
{
    if (m_singleTestTimer) {
        m_singleTestTimer->stop();
        cleanupSingleTimer();
    }

    if (m_allTestRunning) {
        m_allTestRunning = false;
        m_allTestPhase = AllTestPhase::Idle;
        if (m_allTestTimer && m_allTestTimer->isActive())
            m_allTestTimer->stop();
    }

    if (m_noMeasTimer) {
        m_noMeasTimer->stop();
        cleanupNoMeasTimer();
    }

    if (m_io) {
        m_io->requestSingleLineTestStop();
        m_io->requestStepTestStop();
        m_io->requestNoMeasTestStop();
    }

    if (m_active != Active::NoMeas) {
        endMeasurements();
        allLineStatusReturn();

        // страховка от зависших линий в статусе Test
        if (m_model) {
            const int count = m_model->rowCount();
            for (int i = 0; i < count; ++i) {
                Line *ln = m_model->line(i);
                if (!ln)
                    continue;

                if (ln->status() == Line::Test) {
                    ln->setStatus(safeRestoreStatus(ln->oldStatus()));
                }
            }
        }
    }

    m_currentAllIsLong = false;
    setActive(Active::None);
    setSource(Source::None);
    setMeasuredLine(-1);
    setCurrentTestKind(TestKind::None);
    clearTestTiming();
}

void TestController::cleanupSingleTimer()
{
    if (!m_singleTestTimer)
        return;

    m_singleTestTimer->deleteLater();
    m_singleTestTimer = nullptr;
}

void TestController::cleanupNoMeasTimer()
{
    if (!m_noMeasTimer)
        return;

    m_noMeasTimer->deleteLater();
    m_noMeasTimer = nullptr;
}

void TestController::allLineStatusReturn()
{
    if (!m_model)
        return;

    const int count = m_model->rowCount();
    for (int i = 0; i < count; ++i) {
        Line *ln = m_model->line(i);
        if (ln && ln->status() == Line::Test)
            ln->setStatus(safeRestoreStatus(ln->oldStatus()));
    }
}

void TestController::beginMeasurements()
{
    if (m_bus)
        m_bus->setModeTest();
}

void TestController::endMeasurements()
{
    if (m_bus)
        m_bus->setModeNormal();
}

bool TestController::isLineTestable(Line *ln) const
{
    if (!ln)
        return false;

    if (ln->mode() == Line::NotUsed)
        return false;

    return true;
}

int TestController::findNextTestableIndex(int fromExclusive) const
{
    const int count = m_model ? m_model->rowCount() : 0;
    for (int i = fromExclusive + 1; i < count; ++i) {
        if (isLineTestable(m_model->line(i)))
            return i;
    }
    return -1;
}

int TestController::findFirstTestableIndex() const
{
    return findNextTestableIndex(-1);
}

void TestController::startSingleLineTest(int lineIndex, int durationMs)
{

    Line *line = m_model->line(lineIndex);
    if (!isLineTestable(line)) {
        log(QString("Линия %1 недоступна для теста").arg(lineIndex));
        endMeasurements();
        setActive(Active::None);
        setSource(Source::None);
        setMeasuredLine(-1);
        setCurrentTestKind(TestKind::None);
        clearTestTiming();
        return;
    }

    setActive(Active::Single);
    setCurrentTestKind(TestKind::Functional);

    log(QString("ТЕСТ линия: %1 на %2 секунд")
            .arg(line->description())
            .arg(durationMs / 1000));

    const Line::Status prevStatus = safeRestoreStatus(line->status());
    line->setOldStatus(prevStatus);
    line->setStatus(Line::Test);
    line->setPower(0);

    if (!m_io->requestSingleLineTestStart(lineIndex)) {
        line->setStatus(line->oldStatus());
        endMeasurements();
        log(QString("Тест линии %1 не запустился").arg(lineIndex));
        setActive(Active::None);
        setSource(Source::None);
        setCurrentTestKind(TestKind::None);
        clearTestTiming();
        return;
    }

    m_singleTestIndex = lineIndex;
    m_singleTestDurationMs = durationMs;
    setMeasuredLine(-1);
    setTestTiming(durationMs / 1000);

    m_singleTestTimer = new QTimer(this);
    m_singleTestTimer->setSingleShot(true);

    connect(m_singleTestTimer, &QTimer::timeout, this, [this, line]() {
        if (measuredLine() < 0) {
            log("Measurement relay connection timeout; test aborted");
            stopAnyActiveTest();
            return;
        }
        const bool ok = checkLinePowerInternal(line);
        line->setStatus(ok ? Line::OK : Line::Failure);
        line->setLastMeasuredTest(QDateTime::currentDateTime());

        log(QString("⏰ ТЕСТ завершён: %1 mpower=%2 power=%3 tol=%4 -> %5")
                .arg(line->description())
                .arg(line->mpower())
                .arg(line->power())
                .arg(line->tolerance())
                .arg(ok ? "OK" : "Неисправность"));

        if (m_model && !m_linesSavePath.isEmpty())
            m_model->saveToFile(m_linesSavePath);

        if (m_io) {
            m_io->requestSingleLineTestStop();
            setMeasuredLine(-1);
        }

        cleanupSingleTimer();
        setActive(Active::None);
        setSource(Source::None);
        setCurrentTestKind(TestKind::None);
        endMeasurements();
        clearTestTiming();
    });

    m_singleTestTimer->start(10000);
    log(QString("Тест линии %1 старт").arg(lineIndex));
}

void TestController::startAllLinesTest(int durationMs)
{
    m_currentAllAllOk = true;
    m_currentAllIsLong = (durationMs >= m_longThresholdMin * 60 * 1000);

    const int first = findFirstTestableIndex();
    if (first < 0) {
        log("ТЕСТ ВСЕХ: нет линий для теста");
        endMeasurements();
        setActive(Active::None);
        setSource(Source::None);
        setMeasuredLine(-1);
        setCurrentTestKind(TestKind::None);
        clearTestTiming();
        return;
    }

    const int count = m_model->rowCount();
    int countFact = 0;

    for (int i = 0; i < count; ++i) {
        Line *ln = m_model->line(i);
        if (isLineTestable(ln)) {
            const Line::Status prevStatus = safeRestoreStatus(ln->status());
            ln->setOldStatus(prevStatus);
            ln->setStatus(Line::Test);
            ln->setPower(0);
            countFact++;
        }
    }

    setActive(Active::All);
    setCurrentTestKind(m_currentAllIsLong ? TestKind::Duration
                                          : TestKind::Functional);
    m_allTestRunning = true;
    m_allTestCurrentIndex = first;
    m_allTestPhase = AllTestPhase::Warmup;

    durationMs = qMax(durationMs, m_shotTimeWarm * 1000
                     + countFact * allLinesMeasurementWindowMs());
    m_allTestDurationMs = durationMs;
    m_allTestClock.start();
    int warmupMs = durationMs - countFact * allLinesMeasurementWindowMs();
    if (warmupMs < m_shotTimeWarm * 1000)
        warmupMs = m_shotTimeWarm * 1000;
    if (!m_io->requestNoMeasTestStart()) {
        m_allTestRunning = false;
        m_allTestPhase = AllTestPhase::Idle;
        endMeasurements();
        allLineStatusReturn();
        setMeasuredLine(-1);
        setActive(Active::None);
        setSource(Source::None);
        setCurrentTestKind(TestKind::None);
        clearTestTiming();
        return;
    }

    setMeasuredLine(-1);
    setTestTiming((durationMs + 999) / 1000);

    m_allTestTimer->start(warmupMs);
    log(QString("Тест всех линий старт, прогрев %1 ms").arg(warmupMs));
}

void TestController::startCurrentAllLinesMeasurement()
{
    if (!m_allTestRunning || !m_io || m_allTestCurrentIndex < 0)
        return;

    if (!m_io->requestStepTestStart(m_allTestCurrentIndex)) {
        log("⚠️ Не удалось подключить измеритель, прерываем тест");
        stopAnyActiveTest();
        return;
    }

    m_allTestPhase = AllTestPhase::Connecting;
    setMeasuredLine(-1);
    m_allTestTimer->start(10000);
}

void TestController::requestCurrentAllLinesSample()
{
    m_allTestPhase = AllTestPhase::Reading;
    ++m_meterSampleSequence;
    m_allTestTimer->start(5000);
    if (m_bus)
        m_bus->requestTestMeterSample(m_meterSampleSequence);
}

void TestController::onTestMeterSample(quint64 sequence, double voltage, double current, double power)
{
    if (!m_allTestRunning || m_allTestPhase != AllTestPhase::Reading
        || sequence != m_meterSampleSequence)
        return;
    Line *line = m_model ? m_model->line(m_allTestCurrentIndex) : nullptr;
    if (!line || !std::isfinite(voltage) || !std::isfinite(current) || !std::isfinite(power))
        return;
    m_allTestTimer->stop();
    line->setVoltage(voltage);
    line->setCurrent(current);
    line->setPower(power);
    handleAllLinesStep();
}

void TestController::handleAllLinesStep()
{
    if (!m_allTestRunning || !m_model || !m_io)
        return;

    const int idx = m_allTestCurrentIndex;
    Line *line = (idx >= 0 && idx < m_model->rowCount()) ? m_model->line(idx) : nullptr;

    if (isLineTestable(line)) {
        const bool ok = checkLinePowerInternal(line);
        line->setStatus(ok ? Line::OK : Line::Failure);
        line->setLastMeasuredTest(QDateTime::currentDateTime());
        if (!ok)
            m_currentAllAllOk = false;

        log(QString("⏰ Линия %1: mpower=%2 power=%3 tol=%4 -> %5")
                .arg(line->description())
                .arg(line->mpower())
                .arg(line->power())
                .arg(m_currentAllIsLong ? line->tolerance() + m_longTolBonus
                                        : line->tolerance())
                .arg(ok ? "OK" : "Неиспр."));
    }

    const int nextIdx = findNextTestableIndex(m_allTestCurrentIndex);
    if (nextIdx < 0) {
        const qint64 remainingMs = m_allTestDurationMs - m_allTestClock.elapsed();
        if (remainingMs > 0) {
            m_io->requestNoMeasTestStart();
            m_allTestPhase = AllTestPhase::Holding;
            m_allTestTimer->start(int(remainingMs));
        } else {
            finishAllLinesTest();
        }
        return;
    }

    m_allTestCurrentIndex = nextIdx;
    startCurrentAllLinesMeasurement();
}

void TestController::finishAllLinesTest()
{
    m_io->requestStepTestStop();
    m_io->requestNoMeasTestStop();
    log("✅ ТЕСТ всех линий завершён");
    m_allTestRunning = false;
    m_allTestPhase = AllTestPhase::Idle;
    endMeasurements();
    allLineStatusReturn();
    setMeasuredLine(-1);
    setActive(Active::None);
    setSource(Source::None);
    setCurrentTestKind(TestKind::None);
    clearTestTiming();

    if (m_currentAllIsLong)
        recordLongSystemTestResult(m_currentAllAllOk);

    if (m_model && !m_linesSavePath.isEmpty())
        m_model->saveToFile(m_linesSavePath);

    m_currentAllIsLong = false;
}

int TestController::allLinesMeasurementWindowMs() const
{
    const int switchDelayMs = m_io
        ? m_io->measurementSwitchDelayMs()
        : LineIoManager::DefaultMeasurementSwitchDelayMs;
    return m_meterSettlingMs + MeterPollIntervalMs + RelayTransportBudgetMs + switchDelayMs;
}

bool TestController::checkLinePowerInternal(Line *line) const
{
    const double ref  = line->mpower();
    const double meas = line->power();
    if (ref <= 0.0)
        return false;

    double tol = line->tolerance();
    if (m_currentAllIsLong && m_active == Active::All)
        tol = std::min(50.0, tol + m_longTolBonus);

    const double diff    = std::fabs(meas - ref);
    const double maxDiff = ref * tol / 100.0;
    return diff <= maxDiff;
}

void TestController::setMeasuredLine(int idx)
{
    if (m_measuredLine == idx)
        return;

    m_measuredLine = idx;
    emit measuredLineChanged(idx);
}

void TestController::setSource(Source s)
{
    if (m_source == s)
        return;

    m_source = s;
    emit testSourceChanged();
}

void TestController::setActive(Active a)
{
    if (m_active == a)
        return;

    m_active = a;
    emit testActiveChanged();
}

void TestController::recordLongSystemTestResult(bool ok)
{
    m_lastLongSystemTest = QDateTime::currentDateTime();
    m_lastLongSystemTestOk = ok;
    emit lastLongSystemTestChanged();

    log(QString("🧾 Длинный тест шкафа: %1").arg(ok ? "PASS" : "Неиспр."));

    QJsonObject obj;
    obj["lastLongSystemTest"] = m_lastLongSystemTest.toString(Qt::ISODate);
    obj["lastLongSystemTestOk"] = m_lastLongSystemTestOk;

    QFile f(m_maintenancePath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        f.close();
    }
}

void TestController::loadMaintenance()
{
    QFile f(m_maintenancePath);
    if (!f.open(QIODevice::ReadOnly))
        return;

    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    if (!doc.isObject())
        return;

    const auto obj = doc.object();
    m_lastLongSystemTest = QDateTime::fromString(obj.value("lastLongSystemTest").toString(), Qt::ISODate);
    m_lastLongSystemTestOk = obj.value("lastLongSystemTestOk").toBool(true);
}

int TestController::calcAllLinesTestDurationSec() const
{
    if (!m_model)
        return 0;

    int countFact = 0;
    for (int i = 0; i < m_model->rowCount(); ++i) {
        if (isLineTestable(m_model->line(i)))
            countFact++;
    }

    if (countFact <= 0)
        return 0;

    const qint64 totalMs = qint64(m_shotTimeWarm) * 1000
        + qint64(countFact) * allLinesMeasurementWindowMs();
    return int((totalMs + 999) / 1000);
}
