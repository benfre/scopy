#include "logic_analyzer.h"

#include "ui_logic_analyzer.h"
#include "ui_cursors_settings.h"
#include "oscilloscope_plot.hpp"

#include <libsigrokdecode/libsigrokdecode.h>

#include "logicanalyzer/logicdatacurve.h"
#include "logicanalyzer/annotationcurve.h"
#include "logicanalyzer/decoder.h"

#include "basemenu.h"
#include "logicgroupitem.h"
#include "logicanalyzer_api.h"

#include "dynamicWidget.hpp"

#include <QDebug>

#include "filter.hpp"

using namespace adiscope;
using namespace adiscope::logic;

constexpr int MAX_BUFFER_SIZE_ONESHOT = 4 * 1024 * 1024; // 4M
constexpr int MAX_BUFFER_SIZE_STREAM = 1 * 1024 * 1024 * 1024; // 1G

LogicAnalyzer::LogicAnalyzer(iio_context *ctx, adiscope::Filter *filt,
			     adiscope::ToolMenuItem *toolMenuItem,
			     QJSEngine *engine, adiscope::ToolLauncher *parent,
			     bool offline_mode_):
	Tool(ctx, toolMenuItem, new LogicAnalyzer_API(this), "Logic Analyzer", parent),
	ui(new Ui::LogicAnalyzer),
	m_plot(this, 16, 10),
	m_bufferPreviewer(new DigitalBufferPreviewer(40, this)),
	m_sampleRateButton(new ScaleSpinButton({
					{"Hz", 1E0},
					{"kHz", 1E+3},
					{"MHz", 1E+6}
					}, tr("Sample Rate"), 1,
					10e7,
					true, false, this, {1, 2, 5})),
	m_bufferSizeButton(new ScaleSpinButton({
					{"samples", 1E0},
					{"k samples", 1E+3},
					{"M samples", 1E+6},
					{"G samples", 1E+9},
					}, tr("Samples"), 1,
					MAX_BUFFER_SIZE_ONESHOT,
					true, false, this, {1, 2, 5})),
	m_timePositionButton(new PositionSpinButton({
					 {"samples", 1E0},
					 }, tr("Delay"), - (1 << 13),
					 (1 << 13) - 1,
					 true, false, this)),
	m_sampleRate(1.0),
	m_bufferSize(1),
	m_m2kContext(m2kOpen(ctx, "")),
	m_m2kDigital(m_m2kContext->getDigital()),
	m_nbChannels(m_m2kDigital->getNbChannelsIn()),
	m_buffer(nullptr),
	m_horizOffset(0.0),
	m_timeTriggerOffset(0.0),
	m_resetHorizAxisOffset(true),
	m_captureThread(nullptr),
	m_stopRequested(false),
	m_plotScrollBar(new QScrollBar(Qt::Vertical, this)),
	m_started(false),
	m_selectedChannel(-1),
	m_wheelEventGuard(nullptr),
	m_decoderMenu(nullptr),
	m_lastCapturedSample(0),
	m_currentGroupMenu(nullptr),
	m_autoMode(false),
	m_timer(new QTimer(this)),
	m_timerTimeout(1000)
{
	qDebug() << m_m2kDigital << " " << m_m2kContext;

	// setup ui
	setupUi();

	// setup signals slots
	connectSignalsAndSlots();

	m_plot.setLeftVertAxesCount(1);

	// TODO: get number of channels from libm2k;

	for (uint8_t i = 0; i < m_nbChannels; ++i) {
		QCheckBox *channelBox = new QCheckBox("DIO " + QString::number(i));
		ui->channelEnumeratorLayout->addWidget(channelBox, i % 8, i / 8);

		channelBox->setChecked(true);

		// 1 for each channel
		// m_plot.addGenericPlotCurve()
		LogicDataCurve *curve = new LogicDataCurve(nullptr, i, this);
		curve->setTraceHeight(25);
		m_plot.addDigitalPlotCurve(curve, true);

		// use direct connection we want the processing
		// of the available data to be done in the capture thread
		connect(this, &LogicAnalyzer::dataAvailable, this,
			[=](uint64_t from, uint64_t to){
			curve->dataAvailable(from, to);
		}, Qt::DirectConnection);

		m_plotCurves.push_back(curve);

		connect(channelBox, &QCheckBox::toggled, [=](bool toggled){
			m_plot.enableDigitalPlotCurve(i, toggled);
			m_plot.setOffsetWidgetVisible(i, toggled);
			m_plot.positionInGroupChanged(i, 0, 0);
			m_plot.replot();
		});
		channelBox->setChecked(false);
	}

	// Add propper zoomer
	m_plot.addZoomer(0);

	m_plot.setZoomerParams(true, 20);

	m_plot.zoomBaseUpdate();

	connect(&m_plot, &CapturePlot::timeTriggerValueChanged, [=](double value){
		double delay = value / (1.0 / m_sampleRate);
		onTimeTriggerValueChanged(static_cast<int>(delay));
	});


	m_plot.enableXaxisLabels();

//	m_plot.setTimeTriggerInterval(-500000, 500000);

	initBufferScrolling();


	// TODO: scroll area on plot canvas
	// TODO: channel groups
	m_plotScrollBar->setRange(0, 100);

	// setup decoders
	setupDecoders();

	// setup trigger
	setupTriggerMenu();

	m_timePositionButton->setStep(1);

	api->setObjectName(QString::fromStdString(Filter::tool_name(
							  TOOL_LOGIC_ANALYZER)));
	api->load(*settings);
	api->js_register(engine);
}

LogicAnalyzer::~LogicAnalyzer()
{
	if (saveOnExit) {
		api->save(*settings);
	}

	delete api;

	disconnect(prefPanel, &Preferences::notify, this, &LogicAnalyzer::readPreferences);

	for (auto &curve : m_plotCurves) {
		m_plot.removeDigitalPlotCurve(curve);
		delete curve;
	}

	if (m_captureThread) {
		m_stopRequested = true;
		m_m2kDigital->cancelBufferIn();
		m_captureThread->join();
		delete m_captureThread;
		m_captureThread = nullptr;
	}

	if (m_buffer) {
		delete[] m_buffer;
		m_buffer = nullptr;
	}

	if (srd_exit() != SRD_OK) {
	    qDebug() << "Error: srd_exit failed in ~LogicAnalyzer()";
	}

	delete cr_ui;
	delete ui;
}

uint16_t *LogicAnalyzer::getData()
{
	return m_buffer;
}

void LogicAnalyzer::on_btnChannelSettings_toggled(bool checked)
{
	triggerRightMenuToggle(
		static_cast<CustomPushButton *>(QObject::sender()), checked);

	if (checked && m_selectedChannel != -1) {
		ui->nameLineEdit->setText(m_plot.getChannelName(m_selectedChannel));
		ui->traceHeightLineEdit->setText(QString::number(
							 m_plotCurves[m_selectedChannel]->getTraceHeight()));
		if (m_selectedChannel < m_nbChannels) {
			int condition = static_cast<int>(
						m_m2kDigital->getTrigger()->getDigitalCondition(m_selectedChannel));
			ui->triggerComboBox->setCurrentIndex((condition + 1) % 6);
		}
	}
}

void LogicAnalyzer::on_btnCursors_toggled(bool checked)
{
	triggerRightMenuToggle(
		static_cast<CustomPushButton *>(QObject::sender()), checked);
}

void LogicAnalyzer::on_btnTrigger_toggled(bool checked)
{
	triggerRightMenuToggle(
		static_cast<CustomPushButton *>(QObject::sender()), checked);
}

void LogicAnalyzer::on_cursorsBox_toggled(bool on)
{
	m_plot.setCursorReadoutsVisible(on);
	m_plot.setVertCursorsEnabled(on);
}

void LogicAnalyzer::on_btnSettings_clicked(bool checked)
{
	CustomPushButton *btn = nullptr;

	if (checked && !m_menuOrder.isEmpty()) {
		btn = m_menuOrder.back();
		m_menuOrder.pop_back();
	} else {
		btn = static_cast<CustomPushButton *>(
			ui->settings_group->checkedButton());
	}

	btn->setChecked(checked);
}

void LogicAnalyzer::on_btnGeneralSettings_toggled(bool checked)
{
	triggerRightMenuToggle(
		static_cast<CustomPushButton *>(QObject::sender()), checked);
	if(checked)
		ui->btnSettings->setChecked(!checked);
}

void LogicAnalyzer::rightMenuFinished(bool opened)
{
	Q_UNUSED(opened)

	// At the end of each animation, check if there are other button check
	// actions that might have happened while animating and execute all
	// these queued actions
	while (m_menuButtonActions.size()) {
		auto pair = m_menuButtonActions.dequeue();
		toggleRightMenu(pair.first, pair.second);
	}
}

void LogicAnalyzer::onTimeTriggerValueChanged(double value)
{
	if (value > m_timePositionButton->maxValue() ||
			value < m_timePositionButton->minValue()) {
		return;
	}

	m_plot.cancelZoom();
	m_plot.zoomBaseUpdate();

//	qDebug() << "timeTriggermoved sample: " << value << "  time: " << value * (1.0 / m_sampleRate);
	m_plot.setHorizOffset(value * (1.0 / m_sampleRate));
	m_plot.replot();

	if (m_resetHorizAxisOffset) {
		m_horizOffset = value * (1.0 / m_sampleRate);
	}

	m_m2kDigital->getTrigger()->setDigitalDelay(value);

	updateBufferPreviewer();
}

void LogicAnalyzer::onSampleRateValueChanged(double value)
{
	qDebug() << "Sample rate: " << value;
	m_sampleRate = value;

	if (ui->btnStreamOneShot->isChecked()) { // oneshot
		m_plot.cancelZoom();
		m_timePositionButton->setValue(0);
		m_plot.setHorizOffset(value * (1.0 / m_sampleRate));
		m_plot.replot();
		m_plot.zoomBaseUpdate();
	} else { // streaming
		m_plot.cancelZoom();
		m_plot.setHorizOffset(1.0 / m_sampleRate * m_bufferSize / 2.0);
		m_plot.replot();
		m_plot.zoomBaseUpdate();
	}

	m_plot.setHorizUnitsPerDiv(1.0 / m_sampleRate * m_bufferSize / 16.0);

	m_timerTimeout = 1.0 / m_sampleRate * m_bufferSize * 1000.0 + 100;

	m_plot.cancelZoom();
	m_plot.zoomBaseUpdate();
	m_plot.replot();

	updateBufferPreviewer();

	double minT = -(1 << 13) * (1.0 / m_sampleRate); // 8192 * time between samples
	double maxT = ((1 << 13) - 1) * (1.0 / m_sampleRate); // (2 << 13) - 1 max hdl fifo depth
	m_plot.setTimeTriggerInterval(-maxT, -minT);
}

void LogicAnalyzer::onBufferSizeChanged(double value)
{
	qDebug() << "Buffer size: " << value;
	m_bufferSize = value;

	if (ui->btnStreamOneShot->isChecked()) { // oneshot
		m_plot.cancelZoom();
		m_timePositionButton->setValue(0);
		m_plot.setHorizOffset(value * (1.0 / m_sampleRate));
		m_plot.replot();
		m_plot.zoomBaseUpdate();
	} else { // streaming
		m_plot.cancelZoom();
		m_plot.setHorizOffset(1.0 / m_sampleRate * m_bufferSize / 2.0);
		m_plot.replot();
		m_plot.zoomBaseUpdate();
	}

	m_plot.setHorizUnitsPerDiv(1.0 / m_sampleRate * m_bufferSize / 16.0);
	m_timerTimeout = 1.0 / m_sampleRate * m_bufferSize * 1000.0 + 100;

	m_plot.cancelZoom();
	m_plot.zoomBaseUpdate();
	m_plot.replot();

	updateBufferPreviewer();
}

void LogicAnalyzer::on_btnStreamOneShot_toggled(bool toggled)
{
	qDebug() << "Btn stream one shot toggled !!!!!: " << toggled;

	m_plot.enableTimeTrigger(toggled);
	m_timePositionButton->setVisible(toggled);

	m_m2kDigital->getTrigger()->setDigitalStreamingFlag(toggled);

	if (toggled) { // oneshot
		m_plot.cancelZoom();
		m_timePositionButton->setValue(0);
		m_plot.setHorizOffset(0);
		m_plot.replot();
		m_plot.zoomBaseUpdate();
	} else { // streaming
		m_plot.cancelZoom();
		m_plot.setHorizUnitsPerDiv(1.0 / m_sampleRate * m_bufferSize / 16.0);
		m_plot.setHorizOffset(1.0 / m_sampleRate * m_bufferSize / 2.0);
		m_plot.replot();
		m_plot.zoomBaseUpdate();
	}

	m_bufferSizeButton->setMaxValue(toggled ? MAX_BUFFER_SIZE_ONESHOT
						: MAX_BUFFER_SIZE_STREAM);
}

void LogicAnalyzer::on_btnGroupChannels_toggled(bool checked)
{
	qDebug() << checked;
	ui->btnGroupChannels->setText(checked ? "Done" : "Group");

	if (checked) {
		m_plot.beginGroupSelection();
	} else {
		if (m_plot.endGroupSelection()) {
			channelSelectedChanged(m_selectedChannel, false);
		}
	}
}

void LogicAnalyzer::channelSelectedChanged(int chIdx, bool selected)
{
	QSignalBlocker nameLineEditBlocker(ui->nameLineEdit);
	QSignalBlocker traceHeightLineEditBlocker(ui->traceHeightLineEdit);
	QSignalBlocker triggerComboBoxBlocker(ui->triggerComboBox);
	if (m_selectedChannel != chIdx && selected) {

		if (!ui->btnChannelSettings->isChecked()) {
			ui->btnChannelSettings->setChecked(true);
		}

		qDebug() << "Selected channel: " << chIdx;

		m_selectedChannel = chIdx;
		ui->nameLineEdit->setEnabled(true);
		ui->nameLineEdit->setText(m_plotCurves[m_selectedChannel]->getName());
		ui->traceHeightLineEdit->setEnabled(true);
		ui->traceHeightLineEdit->setText(
					QString::number(m_plotCurves[m_selectedChannel]->getTraceHeight()));
		ui->triggerComboBox->setEnabled(true);

		qDebug() << "SIze of group for this channel is: " << m_plot.getGroupOfChannel(m_selectedChannel).size();

		updateChannelGroupWidget(true);

		if (m_selectedChannel < m_nbChannels) {
			ui->triggerComboBox->setVisible(true);
			ui->labelTrigger->setVisible(true);
			int condition = static_cast<int>(
						m_m2kDigital->getTrigger()->getDigitalCondition(m_selectedChannel));
			ui->triggerComboBox->setCurrentIndex((condition + 1) % 6);

			if (m_decoderMenu) {
				ui->decoderSettingsLayout->removeWidget(m_decoderMenu);
				m_decoderMenu->deleteLater();
				m_decoderMenu = nullptr;
			}

			updateStackDecoderButton();

		} else {
			ui->triggerComboBox->setVisible(false);
			ui->labelTrigger->setVisible(false);
			if (m_decoderMenu) {
				ui->decoderSettingsLayout->removeWidget(m_decoderMenu);
				m_decoderMenu->deleteLater();
				m_decoderMenu = nullptr;
			}
			AnnotationCurve *annCurve = dynamic_cast<AnnotationCurve *>(m_plotCurves[m_selectedChannel]);
			m_decoderMenu = annCurve->getCurrentDecoderStackMenu();
			ui->decoderSettingsLayout->addWidget(m_decoderMenu);

			updateStackDecoderButton();
		}
	} else if (m_selectedChannel == chIdx && !selected) {
		m_selectedChannel = -1;
		ui->nameLineEdit->setDisabled(true);
		ui->nameLineEdit->setText("");
		ui->traceHeightLineEdit->setDisabled(true);
		ui->traceHeightLineEdit->setText(QString::number(1));
		ui->triggerComboBox->setDisabled(true);
		ui->triggerComboBox->setCurrentIndex(0);


		if (m_decoderMenu) {
			ui->decoderSettingsLayout->removeWidget(m_decoderMenu);
			m_decoderMenu->deleteLater();
			m_decoderMenu = nullptr;
		}

		updateStackDecoderButton();

		updateChannelGroupWidget(false);
	}
}

void LogicAnalyzer::setupUi()
{
	ui->setupUi(this);

	// Hide the run button
	ui->runSingleWidget->enableRunButton(false);

	int gsettings_panel = ui->stackedWidget->indexOf(ui->generalSettings);
	ui->btnGeneralSettings->setProperty("id", QVariant(-gsettings_panel));

	/* Cursors Settings */
	ui->btnCursors->setProperty("id", QVariant(-1));

	/* Trigger Settings */
	int triggers_panel = ui->stackedWidget->indexOf(ui->triggerSettings);
	ui->btnTrigger->setProperty("id", QVariant(-triggers_panel));

	/* Channel Settings */
	int channelSettings_panel = ui->stackedWidget->indexOf(ui->channelSettings);
	ui->btnChannelSettings->setProperty("id", QVariant(-channelSettings_panel));

	// default trigger menu?
	m_menuOrder.push_back(ui->btnTrigger);

	// set default menu width to 0
	ui->rightMenu->setMaximumWidth(0);

	// Plot positioning and settings
	m_plot.disableLegend();

	QSpacerItem *plotSpacer = new QSpacerItem(0, 5,
		QSizePolicy::Fixed, QSizePolicy::Fixed);

//	ui->gridLayoutPlot->addWidget(measurePanel, 0, 1, 1, 1);
	ui->gridLayoutPlot->addWidget(m_plot.topArea(), 0, 0, 1, 4);
	ui->gridLayoutPlot->addWidget(m_plot.topHandlesArea(), 1, 0, 1, 4);

	ui->gridLayoutPlot->addWidget(m_plot.leftHandlesArea(), 0, 0, 4, 1);
	ui->gridLayoutPlot->addWidget(m_plot.rightHandlesArea(), 0, 3, 4, 1);


	ui->gridLayoutPlot->addWidget(&m_plot, 2, 1, 1, 1);
	ui->gridLayoutPlot->addWidget(m_plotScrollBar, 2, 2, 1, 1);

	ui->gridLayoutPlot->addWidget(m_plot.bottomHandlesArea(), 3, 0, 1, 4);
	ui->gridLayoutPlot->addItem(plotSpacer, 4, 0, 1, 4);
//	ui->gridLayoutPlot->addWidget(statisticsPanel, 6, 1, 1, 1);

	m_plot.enableAxis(QwtPlot::yLeft, false);
	m_plot.enableAxis(QwtPlot::xBottom, false);

	m_plot.setUsingLeftAxisScales(false);
	m_plot.enableLabels(false);

	// Buffer previewer

	m_bufferPreviewer->setVerticalSpacing(6);
	m_bufferPreviewer->setMinimumHeight(20);
	m_bufferPreviewer->setMaximumHeight(20);
	m_bufferPreviewer->setMinimumWidth(375);
//	m_bufferPreviewer->setMaximumWidth(375);
	m_bufferPreviewer->setCursorPos(0.5);

	m_bufferPreviewer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	ui->vLayoutBufferSlot->addWidget(m_bufferPreviewer);

	m_plot.canvas()->setStyleSheet("background-color: #272730");


	// Setup sweep settings menu

	ui->sweepSettingLayout->addWidget(m_sampleRateButton);
	ui->sweepSettingLayout->addWidget(m_bufferSizeButton);
	ui->sweepSettingLayout->addWidget(m_timePositionButton);

	// Setup trigger menu

	// Setup channel / decoder menu


	// Setup cursors menu

	cr_ui = new Ui::CursorsSettings;
	cr_ui->setupUi(ui->cursorsSettings);

	setDynamicProperty(cr_ui->btnLockHorizontal, "use_icon", true);

	auto cursorsPositionButton = new CustomPlotPositionButton(cr_ui->posSelect);
	connect(cursorsPositionButton, &CustomPlotPositionButton::positionChanged,
		[=](CustomPlotPositionButton::ReadoutsPosition position){
		m_plot.moveCursorReadouts(position);
	});

	// Disable some options we don't need for this cursor settings panel
	cr_ui->btnNormalTrack->setVisible(false);
	cr_ui->label_3->setVisible(false);
	cr_ui->line_3->setVisible(false);
	cr_ui->vCursorsEnable->setVisible(false);
	cr_ui->btnLockVertical->setVisible(false);

	cr_ui->horizontalSlider->setMaximum(100);
	cr_ui->horizontalSlider->setMinimum(0);
	cr_ui->horizontalSlider->setSingleStep(1);
	cr_ui->horizontalSlider->setSliderPosition(0);

	ui->triggerComboBox->setDisabled(true);
	ui->nameLineEdit->setDisabled(true);
	ui->traceHeightLineEdit->setDisabled(true);

	ui->traceHeightLineEdit->setValidator(new QIntValidator(1, 100, ui->traceHeightLineEdit));
	ui->traceHeightLineEdit->setText(QString::number(1));

	// Scroll wheel event filter
	m_wheelEventGuard = new MouseWheelWidgetGuard(ui->mainWidget);
	m_wheelEventGuard->installEventRecursively(ui->mainWidget);

	ui->groupWidget->setVisible(false);
	ui->stackDecoderWidget->setVisible(false);
}

void LogicAnalyzer::connectSignalsAndSlots()
{
	// connect all the signals and slots here

	connect(ui->runSingleWidget, &RunSingleWidget::toggled,
		[=](bool checked){
		auto btn = dynamic_cast<CustomPushButton *>(run_button);
		btn->setChecked(checked);
	});
	connect(run_button, &QPushButton::toggled,
		ui->runSingleWidget, &RunSingleWidget::toggle);
	connect(ui->runSingleWidget, &RunSingleWidget::toggled,
		this, &LogicAnalyzer::startStop);


	connect(ui->rightMenu, &MenuAnim::finished,
		this, &LogicAnalyzer::rightMenuFinished);


	// TODO: can be moved away from here into on_cursorsBox_toggled
	connect(ui->cursorsBox, &QCheckBox::toggled, [=](bool toggled){
		if (!toggled) {
			// make sure to deselect the cursors button if
			// the cursors are disabled
			ui->btnCursors->setChecked(false);

			// we also remove the button from the history
			// so that the last menu opened button on top
			// won't open the cursors menu when it is disabled
			m_menuOrder.removeOne(ui->btnCursors);
		}
	});

	connect(&m_plot, &CapturePlot::plotSizeChanged, [=](){
		m_bufferPreviewer->setFixedWidth(m_plot.canvas()->size().width());
		m_plotScrollBar->setFixedHeight(m_plot.canvas()->size().height());
	});

	// some conenctions for the cursors menu
	connect(cr_ui->hCursorsEnable, &CustomSwitch::toggled,
		&m_plot, &CapturePlot::setVertCursorsEnabled);
	connect(cr_ui->btnLockHorizontal, &QPushButton::toggled,
		&m_plot, &CapturePlot::setHorizCursorsLocked);

	connect(cr_ui->horizontalSlider, &QSlider::valueChanged, [=](int value){
		cr_ui->transLabel->setText(tr("Transparency ") + QString::number(value) + "%");
		m_plot.setCursorReadoutsTransparency(value);
	});

	connect(m_plot.getZoomer(), &OscPlotZoomer::zoomFinished, [=](bool isZoomOut){
		updateBufferPreviewer();
	});

	connect(m_sampleRateButton, &ScaleSpinButton::valueChanged,
		this, &LogicAnalyzer::onSampleRateValueChanged);
	connect(m_bufferSizeButton, &ScaleSpinButton::valueChanged,
		this, &LogicAnalyzer::onBufferSizeChanged);

	connect(&m_plot, &CapturePlot::timeTriggerValueChanged, [=](double value){
		double delay = value / (1.0 / m_sampleRate);
		m_timePositionButton->setValue(static_cast<int>(delay));
	});

	connect(m_timePositionButton, &PositionSpinButton::valueChanged,
		this, &LogicAnalyzer::onTimeTriggerValueChanged);


	connect(m_plotScrollBar, &QScrollBar::valueChanged, [=](double value) {
		m_plot.setYaxis(-5 - (value * 0.05), 5 - (value * 0.05));
		m_plot.replot();
	});

	connect(&m_plot, &CapturePlot::channelSelected,
		this, &LogicAnalyzer::channelSelectedChanged);

	connect(ui->nameLineEdit, &QLineEdit::textChanged, [=](const QString &text){
		m_plot.setChannelName(text, m_selectedChannel);
		m_plotCurves[m_selectedChannel]->setName(text);
		if (m_selectedChannel < m_nbChannels) {
			QWidget *widgetInLayout = ui->channelEnumeratorLayout->itemAtPosition(m_selectedChannel % 8,
								    m_selectedChannel / 8)->widget();
			auto channelBox = dynamic_cast<QCheckBox *>(widgetInLayout);
			channelBox->setText(text);
		} else {
			const int selectedDecoder = m_selectedChannel - m_nbChannels;
			QWidget *widgetInLayout = ui->decoderEnumeratorLayout->itemAtPosition(selectedDecoder / 2,
								    selectedDecoder % 2)->widget();
			auto decoderBox = dynamic_cast<QCheckBox *>(widgetInLayout);
			decoderBox->setText(text);
		}
	});

	connect(ui->traceHeightLineEdit, &QLineEdit::textChanged, [=](const QString &text){
		auto validator = ui->traceHeightLineEdit->validator();
		QString toCheck = text;
		int pos;

		setDynamicProperty(ui->traceHeightLineEdit,
				   "invalid",
				   validator->validate(toCheck, pos) == QIntValidator::Intermediate);
	});

	connect(ui->traceHeightLineEdit, &QLineEdit::editingFinished, [=](){
		int value = ui->traceHeightLineEdit->text().toInt();
		m_plotCurves[m_selectedChannel]->setTraceHeight(value);
		m_plot.replot();
		m_plot.positionInGroupChanged(m_selectedChannel, 0, 0);
	});

	connect(ui->triggerComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int index) {
		m_m2kDigital->getTrigger()->setDigitalCondition(m_selectedChannel,
								static_cast<libm2k::M2K_TRIGGER_CONDITION_DIGITAL>((index + 5) % 6));
	});

	connect(ui->stackDecoderComboBox, &QComboBox::currentTextChanged, [=](const QString &text) {
		if (m_selectedChannel < m_nbChannels) {
			return;
		}

		if (m_selectedChannel > m_plotCurves.size() - 1) {
			return;
		}

		if (!ui->stackDecoderComboBox->currentIndex()) {
			return;
		}

		AnnotationCurve *curve = dynamic_cast<AnnotationCurve *>(m_plotCurves[m_selectedChannel]);

		if (!curve) {
			return;
		}

		GSList *dl = g_slist_copy((GSList *)srd_decoder_list());
		for (const GSList *sl = dl; sl; sl = sl->next) {
		    srd_decoder *dec = (struct srd_decoder *)sl->data;
		    if (QString::fromUtf8(dec->id) == text) {
			curve->stackDecoder(std::make_shared<logic::Decoder>(dec));
			break;
		    }
		}

		// Update decoder menu. New decoder must be shown
		// and it might also have some options that can
		// be modified
		if (m_decoderMenu) {
			ui->decoderSettingsLayout->removeWidget(m_decoderMenu);
			m_decoderMenu->deleteLater();
			m_decoderMenu = nullptr;
		}

		m_decoderMenu = curve->getCurrentDecoderStackMenu();
		ui->decoderSettingsLayout->addWidget(m_decoderMenu);

		updateStackDecoderButton();
	});

	connect(ui->printBtn, &QPushButton::clicked, [=](){
		m_plot.printWithNoBackground("Logic Analyzer");
	});
}

void LogicAnalyzer::triggerRightMenuToggle(CustomPushButton *btn, bool checked)
{
	// Queue the action, if right menu animation is in progress. This way
	// the action will be remembered and performed right after the animation
	// finishes
	if (ui->rightMenu->animInProgress()) {
		m_menuButtonActions.enqueue(
			QPair<CustomPushButton *, bool>(btn, checked));
	} else {
		toggleRightMenu(btn, checked);
	}
}

void LogicAnalyzer::toggleRightMenu(CustomPushButton *btn, bool checked)
{

	qDebug() << "toggleRightMenu called!";

	int id = btn->property("id").toInt();

	if (id != -ui->stackedWidget->indexOf(ui->generalSettings)){
		if (!m_menuOrder.contains(btn)){
			m_menuOrder.push_back(btn);
		} else {
			m_menuOrder.removeOne(btn);
			m_menuOrder.push_back(btn);
		}
	}

	if (checked) {
		settingsPanelUpdate(id);
	}

	ui->rightMenu->toggleMenu(checked);
}

void LogicAnalyzer::settingsPanelUpdate(int id)
{
	if (id >= 0) {
		ui->stackedWidget->setCurrentIndex(0);
	} else {
		ui->stackedWidget->setCurrentIndex(-id);
	}

	for (int i = 0; i < ui->stackedWidget->count(); i++) {
		QSizePolicy::Policy policy = QSizePolicy::Ignored;

		if (i == ui->stackedWidget->currentIndex()) {
			policy = QSizePolicy::Expanding;
		}
		QWidget *widget = ui->stackedWidget->widget(i);
		widget->setSizePolicy(policy, policy);
	}
	ui->stackedWidget->adjustSize();
}

void LogicAnalyzer::updateBufferPreviewer()
{
	// Time interval within the plot canvas
	QwtInterval plotInterval = m_plot.axisInterval(QwtPlot::xBottom);

	// Time interval that represents the captured data
	QwtInterval dataInterval(0.0, 0.0);
	long long totalSamples = m_bufferSize;

	if (totalSamples > 0) {
		dataInterval.setMinValue(-((m_bufferSize / m_sampleRate) / 2.0 - (m_timePositionButton->value() * (1.0 / m_sampleRate))));
		dataInterval.setMaxValue((m_bufferSize / m_sampleRate) / 2.0 + (m_timePositionButton->value() * (1.0 / m_sampleRate)));
	}

	// Use the two intervals to determine the width and position of the
	// waveform and of the highlighted area
	QwtInterval fullInterval = plotInterval | dataInterval;
	double wPos = 1 - (fullInterval.maxValue() - dataInterval.minValue()) /
		fullInterval.width();
	double wWidth = dataInterval.width() / fullInterval.width();

	double hPos = 1 - (fullInterval.maxValue() - plotInterval.minValue()) /
		fullInterval.width();
	double hWidth = plotInterval.width() / fullInterval.width();

	// Determine the cursor position
	QwtInterval containerInterval = (totalSamples > 0) ? dataInterval :
		fullInterval;
	double containerWidth = (totalSamples > 0) ? wWidth : 1;
	double containerPos = (totalSamples > 0) ? wPos : 0;
	double cPosInContainer = 1 - (containerInterval.maxValue() - 0) /
		containerInterval.width();
	double cPos = cPosInContainer * containerWidth + containerPos;

	// Update the widget
	m_bufferPreviewer->setWaveformWidth(wWidth);
	m_bufferPreviewer->setWaveformPos(wPos);
	m_bufferPreviewer->setHighlightWidth(hWidth);
	m_bufferPreviewer->setHighlightPos(hPos);
	m_bufferPreviewer->setCursorPos(cPos);
}

void LogicAnalyzer::initBufferScrolling()
{
	// TODO: remove extra signal for plot size changeee!!!!!!!

	connect(m_plot.getZoomer(), &OscPlotZoomer::zoomFinished, [=](bool isZoomOut){
		m_horizOffset = m_plot.HorizOffset();
	});

	connect(m_bufferPreviewer, &BufferPreviewer::bufferMovedBy, [=](int value) {
		m_resetHorizAxisOffset = false;
		double moveTo = 0.0;
		auto interval = m_plot.axisInterval(QwtPlot::xBottom);
		double min = interval.minValue();
		double max = interval.maxValue();
		int width = m_bufferPreviewer->width();
		double xAxisWidth = max - min;

		moveTo = value * xAxisWidth / width;
		m_plot.setHorizOffset(moveTo + m_horizOffset);
		m_plot.replot();
		updateBufferPreviewer();
	});
	connect(m_bufferPreviewer, &BufferPreviewer::bufferStopDrag, [=](){
		m_horizOffset = m_plot.HorizOffset();
		m_resetHorizAxisOffset = true;
	});
	connect(m_bufferPreviewer, &BufferPreviewer::bufferResetPosition, [=](){
		m_plot.setHorizOffset(m_timeTriggerOffset);
		m_plot.replot();
		updateBufferPreviewer();
		m_horizOffset = m_timeTriggerOffset;
	});
}

void LogicAnalyzer::startStop(bool start)
{
	if (m_started == start) {
		return;
	}

	m_started = start;

	if (start) {
		m_stopRequested = false;

		m_m2kDigital->flushBufferIn();

		const double sampleRate = m_sampleRateButton->value();
		const uint64_t bufferSize = m_bufferSizeButton->value();
		const int bufferSizeAdjusted = static_cast<int>(((bufferSize + 3 ) / 4) * 4);
		m_bufferSizeButton->setValue(bufferSizeAdjusted);

		const bool oneShotOrStream = ui->btnStreamOneShot->isChecked();
		qDebug() << "stream one shot is set to: " << oneShotOrStream;

		const double delay = oneShotOrStream ? m_timePositionButton->value()
					       : (m_bufferSize / 2.0);

		const double setSampleRate = m_m2kDigital->setSampleRateIn(sampleRate);
		m_sampleRateButton->setValue(setSampleRate);

		m_m2kDigital->getTrigger()->setDigitalStreamingFlag(!oneShotOrStream);

		for (int i = 0; i < m_plotCurves.size(); ++i) {
			QwtPlotCurve *curve = m_plot.getDigitalPlotCurve(i);
			GenericLogicPlotCurve *logic_curve = dynamic_cast<GenericLogicPlotCurve *>(curve);
			logic_curve->reset();

			logic_curve->setSampleRate(sampleRate);
			logic_curve->setBufferSize(bufferSizeAdjusted);
			logic_curve->setTimeTriggerOffset(delay);
		}

		m_lastCapturedSample = 0;

		if (m_autoMode) {

			m_plot.setTriggerState(CapturePlot::Auto);

			double oneBufferTimeOut = m_timerTimeout;

			if (!oneShotOrStream) {
				uint64_t chunks = 4;
				while ((bufferSizeAdjusted >> chunks) > (1 << 19)) {
					chunks++; // select a small size for the chunks
					// example: 2^19 samples in each chunk
				}
				const uint64_t chunk_size = (bufferSizeAdjusted >> chunks) > 0 ? (bufferSizeAdjusted >> chunks) : 4 ;
				const uint64_t buffersCount = bufferSizeAdjusted / chunk_size;
				oneBufferTimeOut /= buffersCount;
			}

			m_timer->start(oneBufferTimeOut);
		}

		m_captureThread = new std::thread([=](){

			if (m_buffer) {
				delete[] m_buffer;
				m_buffer = nullptr;
			}

			m_buffer = new uint16_t[bufferSize];

			QMetaObject::invokeMethod(this, [=](){
				m_plot.setTriggerState(CapturePlot::Waiting);
			}, Qt::QueuedConnection);

			if (ui->btnStreamOneShot->isChecked()) {
				try {
					const uint16_t * const temp = m_m2kDigital->getSamplesP(bufferSize);
					memcpy(m_buffer, temp, bufferSizeAdjusted * sizeof(uint16_t));

					QMetaObject::invokeMethod(this, [=](){
						m_plot.setTriggerState(CapturePlot::Triggered);
					}, Qt::QueuedConnection);

				} catch (std::invalid_argument &e) {
					qDebug() << e.what();
				}

				Q_EMIT dataAvailable(0, bufferSize);

			} else {
				uint64_t chunks = 4;
				while ((bufferSizeAdjusted >> chunks) > (1 << 19)) {
					chunks++; // select a small size for the chunks
					// example: 2^19 samples in each chunk
				}
				const uint64_t chunk_size = (bufferSizeAdjusted >> chunks) > 0 ? (bufferSizeAdjusted >> chunks) : 4 ;
				uint64_t totalSamples = bufferSizeAdjusted;
				m_m2kDigital->setKernelBuffersCountIn(64);
				uint64_t absIndex = 0;

				do {
					const uint64_t captureSize = std::min(chunk_size, totalSamples);
					try {
						const uint16_t * const temp = m_m2kDigital->getSamplesP(captureSize);
						memcpy(m_buffer + absIndex, temp, sizeof(uint16_t) * captureSize);
						absIndex += captureSize;
						totalSamples -= captureSize;

						QMetaObject::invokeMethod(this, [=](){
							m_plot.setTriggerState(CapturePlot::Triggered);
						}, Qt::QueuedConnection);

					} catch (std::invalid_argument &e) {
						qDebug() << e.what();
					}

					if (m_stopRequested) {
						break;
					}

					Q_EMIT dataAvailable(absIndex - captureSize, absIndex);

					QMetaObject::invokeMethod(&m_plot, // trigger replot on Main Thread
								  "replot",
								  Qt::QueuedConnection);
					m_lastCapturedSample = absIndex;

				} while (totalSamples);
			}

			m_started = false;

			QMetaObject::invokeMethod(this,
						  "restoreTriggerState",
						  Qt::DirectConnection);

			//
			QMetaObject::invokeMethod(ui->runSingleWidget,
						  "toggle",
						  Qt::QueuedConnection,
						  Q_ARG(bool, false));

			QMetaObject::invokeMethod(&m_plot,
						  "replot");

			QMetaObject::invokeMethod(this, [=](){
				m_plot.setTriggerState(CapturePlot::Stop);
			}, Qt::QueuedConnection);

		});

	} else {
		if (m_captureThread) {
			m_stopRequested = true;
			m_m2kDigital->cancelBufferIn();
			m_captureThread->join();
			delete m_captureThread;
			m_captureThread = nullptr;
			restoreTriggerState();

			m_plot.setTriggerState(CapturePlot::Stop);
		}
	}

}

static gint sort_pds(gconstpointer a, gconstpointer b)
{
    const struct srd_decoder *sda, *sdb;

    sda = (const struct srd_decoder *)a;
    sdb = (const struct srd_decoder *)b;
    return strcmp(sda->id, sdb->id);
}

void LogicAnalyzer::setupDecoders()
{
	if (srd_init(nullptr) != SRD_OK) {
		qDebug() << "Error: libsigrokdecode init failed!";
	}

	if (srd_decoder_load_all() != SRD_OK) {
		qDebug() << "Error: srd_decoder_load_all failed!";
	}

	ui->addDecoderComboBox->addItem("Select a decoder to add");


	GSList *decoderList = g_slist_copy((GSList *)srd_decoder_list());
	decoderList = g_slist_sort(decoderList, sort_pds);

	for (const GSList *sl = decoderList; sl; sl = sl->next) {

	    srd_decoder *dec = (struct srd_decoder *)sl->data;

	    QString decoderInput = "";

	    GSList *dec_channels = g_slist_copy(dec->inputs);
	    for (const GSList *sl = dec_channels; sl; sl = sl->next) {
		decoderInput = QString::fromUtf8((char*)sl->data);
	    }
	    g_slist_free(dec_channels);

	    if (decoderInput == "logic") {
		ui->addDecoderComboBox->addItem(QString::fromUtf8(dec->id));
	    }
	}

	g_slist_free(decoderList);

	connect(ui->addDecoderComboBox, QOverload<const QString &>::of(&QComboBox::currentIndexChanged), [=](const QString &decoder) {
		if (!ui->addDecoderComboBox->currentIndex()) {
			return;
		}

		std::shared_ptr<logic::Decoder> initialDecoder = nullptr;

		GSList *decoderList = g_slist_copy((GSList *)srd_decoder_list());
		for (const GSList *sl = decoderList; sl; sl = sl->next) {
		    srd_decoder *dec = (struct srd_decoder *)sl->data;
		    if (QString::fromUtf8(dec->id) == decoder) {
			initialDecoder = std::make_shared<logic::Decoder>(dec);
		    }
		}

		g_slist_free(decoderList);

		AnnotationCurve *curve = new AnnotationCurve(this, initialDecoder);
		curve->setTraceHeight(25);
		m_plot.addDigitalPlotCurve(curve, true);

		// use direct connection we want the processing
		// of the available data to be done in the capture thread
		auto connectionHandle = connect(this, &LogicAnalyzer::dataAvailable,
			this, [=](uint64_t from, uint64_t to){
			curve->dataAvailable(from, to);
		}, Qt::DirectConnection);

		curve->setSampleRate(m_sampleRate);
		curve->setBufferSize(m_bufferSize);
		curve->setTimeTriggerOffset(m_timeTriggerOffset);

		curve->dataAvailable(0, m_lastCapturedSample);

		m_plotCurves.push_back(curve);

		QSpacerItem *spacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
		QWidget *decoderMenuItem = new QWidget();
		QHBoxLayout *layout = new QHBoxLayout(decoderMenuItem);
		QCheckBox *decoderBox = new QCheckBox(decoder);
		decoderBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		layout->addWidget(decoderBox);

		QPushButton *deleteBtn = new QPushButton(this);
		deleteBtn->setFlat(true);
		deleteBtn->setIcon(QIcon(":/icons/close.svg"));
		deleteBtn->setMaximumSize(QSize(16, 16));

		layout->addWidget(deleteBtn);
		layout->insertSpacerItem(2, spacer);

		connect(deleteBtn, &QPushButton::clicked, [=](){
			ui->decoderEnumeratorLayout->removeWidget(decoderMenuItem);
			decoderMenuItem->deleteLater();

			int chIdx = m_plotCurves.indexOf(curve);
			bool groupDeleted = false;
			m_plot.removeFromGroup(chIdx,
					       m_plot.getGroupOfChannel(chIdx).indexOf(chIdx),
					       groupDeleted);

			if (groupDeleted) {
				ui->groupWidget->setVisible(false);
				m_currentGroup.clear();
				ui->groupWidgetLayout->removeWidget(m_currentGroupMenu);
				m_currentGroupMenu->deleteLater();
				m_currentGroupMenu = nullptr;
			}

			m_plot.removeDigitalPlotCurve(curve);
			m_plotCurves.removeOne(curve);

			disconnect(connectionHandle);


			delete curve;
		});

		const int itemsInLayout = ui->decoderEnumeratorLayout->count();
		ui->decoderEnumeratorLayout->addWidget(decoderMenuItem, itemsInLayout / 2,
							itemsInLayout % 2);

		ui->addDecoderComboBox->setCurrentIndex(0);

		connect(decoderBox, &QCheckBox::toggled, [=](bool toggled){
			m_plot.enableDigitalPlotCurve(m_nbChannels + itemsInLayout, toggled);
			m_plot.setOffsetWidgetVisible(m_nbChannels + itemsInLayout, toggled);
			m_plot.positionInGroupChanged(m_nbChannels + itemsInLayout, 0, 0);
			m_plot.replot();
		});

		decoderBox->setChecked(true);

		// TODO: not working :(
//		ui->scrollAreaWidgetContents->update();
//		ui->scrollAreaWidgetContents->repaint();
//		ui->scrollAreaWidgetContents->updateGeometry();
//		// Scroll to the bottom when adding new decoder, just to make sure we see
//		// it there (in the menu) after it's added.
//		QScrollBar *generalSettingsMenuScrollBar = ui->generalSettingsScrollArea->verticalScrollBar();
//		const int maxValueOfScrollBar = generalSettingsMenuScrollBar->maximum();
//		generalSettingsMenuScrollBar->setValue(maxValueOfScrollBar);

//		ui->generalSettingsScrollArea->ensureWidgetVisible(decoderBox);
	});

}

void LogicAnalyzer::updateStackDecoderButton()
{
	qDebug() << "updateStackDecoderButton called!";

	if (m_selectedChannel < m_nbChannels) {
		ui->stackDecoderWidget->setVisible(false);
		return;
	}

	if (m_selectedChannel > m_plotCurves.size() - 1) {
		return;
	}

	AnnotationCurve *curve = dynamic_cast<AnnotationCurve *>(m_plotCurves[m_selectedChannel]);

	if (!curve) {
		return;
	}

	auto stack = curve->getDecoderStack();
	auto top = stack.back();

	QSignalBlocker stackDecoderComboBoxBlocker(ui->stackDecoderComboBox);
	ui->stackDecoderComboBox->clear();
	ui->stackDecoderComboBox->addItem("-");

	QString decoderOutput = "";
	GSList *decoderList = g_slist_copy((GSList *)srd_decoder_list());
	decoderList = g_slist_sort(decoderList, sort_pds);
	GSList *dec_channels = g_slist_copy(top->decoder()->outputs);
	for (const GSList *sl = dec_channels; sl; sl = sl->next) {
	    decoderOutput = QString::fromUtf8((char*)sl->data);
	}
	g_slist_free(dec_channels);
	for (const GSList *sl = decoderList; sl; sl = sl->next) {

	    srd_decoder *dec = (struct srd_decoder *)sl->data;

	    QString decoderInput = "";

	    GSList *dec_channels = g_slist_copy(dec->inputs);
	    for (const GSList *sl = dec_channels; sl; sl = sl->next) {
		decoderInput = QString::fromUtf8((char*)sl->data);
	    }
	    g_slist_free(dec_channels);

	    if (decoderInput == decoderOutput) {
		qDebug() << "Added: " << QString::fromUtf8(dec->id);
		ui->stackDecoderComboBox->addItem(QString::fromUtf8(dec->id));
	    }
	}
	g_slist_free(decoderList);

	const bool shouldBeVisible = ui->stackDecoderComboBox->count() > 1;

	ui->stackDecoderWidget->setVisible(shouldBeVisible);
}

void LogicAnalyzer::updateChannelGroupWidget(bool visible)
{

	QVector<int> channelsInGroup = m_plot.getGroupOfChannel(m_selectedChannel);

	const bool shouldBeVisible = visible & (channelsInGroup.size() > 0);

	ui->groupWidget->setVisible(shouldBeVisible);

	qDebug() << "channel group widget should be visible: " << shouldBeVisible
		 << " visible: " << visible << " channelsInGroup: " << channelsInGroup.size();

	if (!shouldBeVisible) {
		return;
	}

	if (channelsInGroup == m_currentGroup) {
		return;
	}

	m_currentGroup = channelsInGroup;

	if (m_currentGroupMenu) {
		ui->groupWidgetLayout->removeWidget(m_currentGroupMenu);
		m_currentGroupMenu->deleteLater();
		m_currentGroupMenu = nullptr;
	}

	m_currentGroupMenu = new BaseMenu(ui->groupWidget);
	ui->groupWidgetLayout->addWidget(m_currentGroupMenu);

	connect(m_currentGroupMenu, &BaseMenu::itemMovedFromTo, [=](short from, short to){
		m_plot.positionInGroupChanged(m_selectedChannel, from, to);
	});

	for (int i = 0; i < channelsInGroup.size(); ++i) {
		QString name = m_plotCurves[channelsInGroup[i]]->getName();
		LogicGroupItem *item = new LogicGroupItem(name, m_currentGroupMenu);
		connect(m_plotCurves[channelsInGroup[i]], &GenericLogicPlotCurve::nameChanged,
				item, &LogicGroupItem::setName);
		connect(item, &LogicGroupItem::deleteBtnClicked, [=](){
			bool groupDeleted = false;
			m_plot.removeFromGroup(m_selectedChannel, item->position(), groupDeleted);

			qDebug() << "m_selectedChannel: " << m_selectedChannel << " deleted: " << m_currentGroup[item->position()];
			if (m_selectedChannel == m_currentGroup[item->position()] && !groupDeleted) {
				ui->groupWidget->setVisible(false);
			}

			m_currentGroup.removeAt(item->position());
			if (groupDeleted) {
				ui->groupWidget->setVisible(false);
				m_currentGroup.clear();
				ui->groupWidgetLayout->removeWidget(m_currentGroupMenu);
				m_currentGroupMenu->deleteLater();
				m_currentGroupMenu = nullptr;
			}
		});
		m_currentGroupMenu->insertMenuItem(item);
	}

	// TODO: fix hardcoded value
	m_currentGroupMenu->setMaximumHeight(channelsInGroup.size() * 27);
}

void LogicAnalyzer::setupTriggerMenu()
{
	/*
	 * TODO: explain some things maybe
	 *
	 *
	 *
	 *
	 * */


	connect(ui->btnTriggerMode, &CustomSwitch::toggled, [=](bool toggled){
		m_autoMode = toggled;

		if (m_autoMode && m_started) {

			double oneBufferTimeOut = m_timerTimeout;

			if (!ui->btnStreamOneShot->isChecked()) {
				uint64_t chunks = 4;
				while ((m_bufferSize >> chunks) > (1 << 19)) {
					chunks++; // select a small size for the chunks
					// example: 2^19 samples in each chunk
				}
				const uint64_t chunk_size = (m_bufferSize >> chunks) > 0 ? (m_bufferSize >> chunks) : 4 ;
				const uint64_t buffersCount = m_bufferSize / chunk_size;
				oneBufferTimeOut /= buffersCount;
			}

			m_timer->start(oneBufferTimeOut);

			qDebug() << "auto mode: " << m_autoMode << " with timeout: "
				 << oneBufferTimeOut << " when logic is started: " << m_started;
		}
	});

	ui->triggerLogicComboBox->addItem("OR");
	ui->triggerLogicComboBox->addItem("AND");

	connect(ui->triggerLogicComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int index){
		m_m2kDigital->getTrigger()->setDigitalMode(static_cast<libm2k::digital::DIO_TRIGGER_MODE>(index));
	});

	ui->externalTriggerSourceComboBox->addItem("External Trigger In");
	ui->externalTriggerSourceComboBox->addItem("Oscilloscope");

	connect(ui->externalTriggerSourceComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int index){
		m_m2kDigital->getTrigger()->setDigitalSource(static_cast<M2K_TRIGGER_SOURCE_DIGITAL>(index));
		if (index) { // oscilloscope
			/* set external trigger condition to none if the source is
			* set to oscilloscope (trigger in)
			* */
			ui->externalTriggerConditionComboBox->setCurrentIndex(0); // None
		}

		/*
		 * Disable the condition combo box if oscilloscope (trigger in) is selected
		 * and enable it if external trigger in is selected (trigger logic)
		 * */
		ui->externalTriggerConditionComboBox->setDisabled(index);
	});

	connect(ui->externalTriggerConditionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int index) {
		m_m2kDigital->getTrigger()->setDigitalExternalCondition(
								static_cast<libm2k::M2K_TRIGGER_CONDITION_DIGITAL>((index + 5) % 6));
	});

	connect(ui->btnEnableExternalTrigger, &CustomSwitch::toggled, [=](bool on){
		if (on) {
			int source = ui->externalTriggerSourceComboBox->currentIndex();
			int condition = ui->externalTriggerConditionComboBox->currentIndex();
			m_m2kDigital->getTrigger()->setDigitalSource(static_cast<M2K_TRIGGER_SOURCE_DIGITAL>(source));
			m_m2kDigital->getTrigger()->setDigitalExternalCondition(
						static_cast<libm2k::M2K_TRIGGER_CONDITION_DIGITAL>((condition + 5) % 6));
		} else {
			m_m2kDigital->getTrigger()->setDigitalSource(M2K_TRIGGER_SOURCE_DIGITAL::SRC_NONE);
		}
	});

	QSignalBlocker blockerExternalTriggerConditionComboBox(ui->externalTriggerConditionComboBox);
	const int condition = static_cast<int>(
				m_m2kDigital->getTrigger()->getDigitalExternalCondition());
	ui->externalTriggerConditionComboBox->setCurrentIndex((condition + 1) % 6);

	QSignalBlocker blockerExternalTriggerSourceComboBox(ui->externalTriggerSourceComboBox);
	ui->externalTriggerSourceComboBox->setCurrentIndex(0);
	m_m2kDigital->getTrigger()->setDigitalSource(M2K_TRIGGER_SOURCE_DIGITAL::SRC_NONE);

	m_timer->setSingleShot(true);
	connect(m_timer, &QTimer::timeout,
		this, &LogicAnalyzer::saveTriggerState);

	m_plot.setTriggerState(CapturePlot::Stop);

}

void LogicAnalyzer::saveTriggerState()
{
	// save trigger state and set to no trigger each channel
	if (m_started) {
		for (int i = 0; i < m_nbChannels; ++i) {
			m_triggerState.push_back(m_m2kDigital->getTrigger()->getDigitalCondition(i));
			m_m2kDigital->getTrigger()->setDigitalCondition(i, M2K_TRIGGER_CONDITION_DIGITAL::NO_TRIGGER_DIGITAL);
		}

		auto externalTriggerCondition = m_m2kDigital->getTrigger()->getDigitalExternalCondition();
		m_triggerState.push_back(externalTriggerCondition);
		m_m2kDigital->getTrigger()->setDigitalExternalCondition(M2K_TRIGGER_CONDITION_DIGITAL::NO_TRIGGER_DIGITAL);
	}
}

void LogicAnalyzer::restoreTriggerState()
{
	// restored saved trigger state
	if (!m_started && m_triggerState.size()) {
		for (int i = 0; i < m_nbChannels; ++i) {
			m_triggerState.push_back(m_m2kDigital->getTrigger()->getDigitalCondition(i));
			m_m2kDigital->getTrigger()->setDigitalCondition(i, m_triggerState[i]);
		}

		m_m2kDigital->getTrigger()->setDigitalExternalCondition(m_triggerState.back());

		m_triggerState.clear();
	}
}

void LogicAnalyzer::readPreferences()
{
	qDebug() << "reading preferences!!!!";
	for (GenericLogicPlotCurve *curve : m_plotCurves) {
		if (curve->getType() == LogicPlotCurveType::Data) {
			LogicDataCurve *ldc = dynamic_cast<LogicDataCurve*>(curve);
			if (!ldc) {
				continue;
			}

			ldc->setDisplaySampling(prefPanel->getDisplaySamplingPoints());
		}
	}

	m_plot.replot();
}
