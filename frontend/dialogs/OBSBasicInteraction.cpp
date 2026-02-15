/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/******************************************************************************
    Modifications Copyright (C) 2026 Uniflow, Inc.
    Author: Kim Taehyung <gaiaengine@gmail.com>
    Modified: 2026-02-15
    Notes: Changes for Syndy Creator Studio.
******************************************************************************/

#include "OBSBasicInteraction.hpp"

#include <dialogs/OBSBasicProperties.hpp>
#include <utility/OBSEventFilter.hpp>
#include <utility/display-helpers.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <QInputEvent>
#include <QLabel>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <Windows.h>
#endif

#include "moc_OBSBasicInteraction.cpp"

using namespace std;

namespace {
struct Scene3DCameraBasis {
	float right_x;
	float right_y;
	float right_z;
	float up_x;
	float up_y;
	float up_z;
	float forward_x;
	float forward_y;
	float forward_z;
};

struct GizmoAxis {
	char label;
	float screen_x;
	float screen_y;
	float depth;
	struct vec4 color;
};

static float VecLength(float x, float y, float z)
{
	return sqrtf(x * x + y * y + z * z);
}

static void NormalizeVec3(float &x, float &y, float &z, float fallback_x, float fallback_y, float fallback_z)
{
	const float length = VecLength(x, y, z);

	if (length > 0.0001f) {
		const float inv_length = 1.0f / length;
		x *= inv_length;
		y *= inv_length;
		z *= inv_length;
		return;
	}

	x = fallback_x;
	y = fallback_y;
	z = fallback_z;
}

static void NormalizeCameraBasis(Scene3DCameraBasis &basis)
{
	NormalizeVec3(basis.right_x, basis.right_y, basis.right_z, 1.0f, 0.0f, 0.0f);
	NormalizeVec3(basis.up_x, basis.up_y, basis.up_z, 0.0f, 1.0f, 0.0f);
	NormalizeVec3(basis.forward_x, basis.forward_y, basis.forward_z, 0.0f, 0.0f, -1.0f);
}

static bool IsScene3DSource(obs_source_t *source)
{
	const char *id = source ? obs_source_get_id(source) : nullptr;

	return id && strcmp(id, "scene_3d_source") == 0;
}

static bool GetScene3DCameraBasis(obs_source_t *source, Scene3DCameraBasis &basis)
{
	proc_handler_t *proc_handler = nullptr;
	calldata_t cd = {};
	bool success = false;

	basis = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f};

	if (!source || !IsScene3DSource(source))
		return false;

	proc_handler = obs_source_get_proc_handler(source);
	if (!proc_handler)
		return false;

	success = proc_handler_call(proc_handler, "get_scene_3d_camera_basis", &cd);
	if (!success) {
		calldata_free(&cd);
		return false;
	}

	if (!calldata_bool(&cd, "available")) {
		calldata_free(&cd);
		return false;
	}

	basis.forward_x = (float)calldata_float(&cd, "forward_x");
	basis.forward_y = (float)calldata_float(&cd, "forward_y");
	basis.forward_z = (float)calldata_float(&cd, "forward_z");
	basis.right_x = (float)calldata_float(&cd, "right_x");
	basis.right_y = (float)calldata_float(&cd, "right_y");
	basis.right_z = (float)calldata_float(&cd, "right_z");
	basis.up_x = (float)calldata_float(&cd, "up_x");
	basis.up_y = (float)calldata_float(&cd, "up_y");
	basis.up_z = (float)calldata_float(&cd, "up_z");
	calldata_free(&cd);

	NormalizeCameraBasis(basis);
	return true;
}

static void DrawGizmoLine(float x1, float y1, float x2, float y2, float thickness)
{
	const float dx = x2 - x1;
	const float dy = y2 - y1;
	const float length = sqrtf(dx * dx + dy * dy);

	if (length <= 0.0001f)
		return;

	gs_matrix_push();
	gs_matrix_translate3f(x1, y1, 0.0f);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, atan2f(dy, dx));
	gs_matrix_translate3f(0.0f, -thickness * 0.5f, 0.0f);
	gs_draw_quadf(NULL, 0.0f, length, thickness);
	gs_matrix_pop();
}

static void DrawGizmoQuad(float x, float y, float width, float height)
{
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_draw_quadf(NULL, 0, width, height);
	gs_matrix_pop();
}

static void DrawGizmoLabelGlyph(gs_effect_t *solid, gs_eparam_t *color_param, char glyph, float x, float y, float size,
				float thickness, const struct vec4 *color)
{
	const float half = size * 0.5f;
	const float join = size * 0.15f;

	if (!solid || !color_param || !color)
		return;

	gs_effect_set_vec4(color_param, color);

	while (gs_effect_loop(solid, "Solid")) {
		switch (glyph) {
		case 'x':
			DrawGizmoLine(x - half, y - half, x + half, y + half, thickness);
			DrawGizmoLine(x - half, y + half, x + half, y - half, thickness);
			break;
		case 'y':
			DrawGizmoLine(x, y + half, x, y - join, thickness);
			DrawGizmoLine(x - half, y - half, x, y - join, thickness);
			DrawGizmoLine(x + half, y - half, x, y - join, thickness);
			break;
		case 'z':
			DrawGizmoLine(x - half, y - half, x + half, y - half, thickness);
			DrawGizmoLine(x + half, y - half, x - half, y + half, thickness);
			DrawGizmoLine(x - half, y + half, x + half, y + half, thickness);
			break;
		default:
			break;
		}
	}
}

static void DrawScene3DGizmo(obs_source_t *source, int viewport_x, int viewport_y, int viewport_cx, int viewport_cy)
{
	Scene3DCameraBasis basis = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f};
	gs_effect_t *solid = nullptr;
	gs_eparam_t *color_param = nullptr;
	std::array<GizmoAxis, 3> axes;
	const int gizmo_size = clamp(min(viewport_cx, viewport_cy) / 4, 96, 168);
	const int gizmo_margin = max(8, gizmo_size / 8);
	const int gizmo_x = viewport_x + viewport_cx - gizmo_size - gizmo_margin;
	const int gizmo_y = viewport_y + gizmo_margin;
	const float center_x = 0.5f;
	const float center_y = 0.5f;
	const float axis_radius = 0.31f;
	const float axis_thickness = 0.028f;
	const float label_size = 0.10f;
	const float label_offset = 0.08f;
	struct vec4 background_color;
	struct vec4 center_color;

	if (!source || !IsScene3DSource(source))
		return;
	if (viewport_cx <= 0 || viewport_cy <= 0)
		return;

	GetScene3DCameraBasis(source, basis);

	solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	color_param = solid ? gs_effect_get_param_by_name(solid, "color") : nullptr;
	if (!solid || !color_param)
		return;

	axes[0].label = 'x';
	axes[0].screen_x = basis.right_x;
	axes[0].screen_y = basis.up_x;
	axes[0].depth = basis.forward_x;
	vec4_set(&axes[0].color, 0.95f, 0.32f, 0.32f, 1.0f);

	axes[1].label = 'y';
	axes[1].screen_x = basis.right_y;
	axes[1].screen_y = basis.up_y;
	axes[1].depth = basis.forward_y;
	vec4_set(&axes[1].color, 0.36f, 0.88f, 0.38f, 1.0f);

	axes[2].label = 'z';
	axes[2].screen_x = basis.right_z;
	axes[2].screen_y = basis.up_z;
	axes[2].depth = basis.forward_z;
	vec4_set(&axes[2].color, 0.38f, 0.55f, 0.98f, 1.0f);

	sort(axes.begin(), axes.end(), [](const GizmoAxis &a, const GizmoAxis &b) { return a.depth > b.depth; });

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();

	gs_set_viewport(gizmo_x, gizmo_y, gizmo_size, gizmo_size);
	/*
	 * Keep overlay coordinates consistent with OBS preview space:
	 * x grows right, y grows down.
	 */
	gs_ortho(0.0f, 1.0f, 0.0f, 1.0f, -100.0f, 100.0f);

	vec4_set(&background_color, 0.05f, 0.06f, 0.08f, 0.58f);
	gs_effect_set_vec4(color_param, &background_color);
	while (gs_effect_loop(solid, "Solid"))
		DrawGizmoQuad(0.02f, 0.02f, 0.96f, 0.96f);

	for (const GizmoAxis &axis : axes) {
		const float end_x = center_x + axis.screen_x * axis_radius;
		const float end_y = center_y - axis.screen_y * axis_radius;
		float dir_x = end_x - center_x;
		float dir_y = end_y - center_y;
		const float dir_len = sqrtf(dir_x * dir_x + dir_y * dir_y);
		float label_x;
		float label_y;

		gs_effect_set_vec4(color_param, &axis.color);
		while (gs_effect_loop(solid, "Solid"))
			DrawGizmoLine(center_x, center_y, end_x, end_y, axis_thickness);

		while (gs_effect_loop(solid, "Solid"))
			DrawGizmoQuad(end_x - 0.015f, end_y - 0.015f, 0.03f, 0.03f);

		if (dir_len > 0.0001f) {
			dir_x /= dir_len;
			dir_y /= dir_len;
		} else {
			dir_x = 0.0f;
			dir_y = -1.0f;
		}

		label_x = end_x + dir_x * label_offset;
		label_y = end_y + dir_y * label_offset;
		DrawGizmoLabelGlyph(solid, color_param, axis.label, label_x, label_y, label_size, axis_thickness * 0.65f,
				    &axis.color);
	}

	vec4_set(&center_color, 0.93f, 0.93f, 0.93f, 1.0f);
	gs_effect_set_vec4(color_param, &center_color);
	while (gs_effect_loop(solid, "Solid"))
		DrawGizmoQuad(center_x - 0.017f, center_y - 0.017f, 0.034f, 0.034f);

	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();

	gs_blend_state_pop();
}
} // namespace

OBSBasicInteraction::OBSBasicInteraction(QWidget *parent, OBSSource source_)
	: QDialog(parent),
	  main(qobject_cast<OBSBasic *>(parent)),
	  ui(new Ui::OBSBasicInteraction),
	  source(source_),
	  removedSignal(obs_source_get_signal_handler(source), "remove", OBSBasicInteraction::SourceRemoved, this),
	  renamedSignal(obs_source_get_signal_handler(source), "rename", OBSBasicInteraction::SourceRenamed, this),
	  eventFilter(BuildEventFilter())
{
	int cx = (int)config_get_int(App()->GetAppConfig(), "InteractionWindow", "cx");
	int cy = (int)config_get_int(App()->GetAppConfig(), "InteractionWindow", "cy");

	Qt::WindowFlags flags = windowFlags();
	flags &= ~Qt::WindowContextHelpButtonHint;
	flags |= Qt::WindowSystemMenuHint;
	flags |= Qt::WindowMinMaxButtonsHint;
	setWindowFlags(flags);

	ui->setupUi(this);
	{
		QLabel *interactionHint = new QLabel(QTStr("Basic.InteractionWindow.Hint"), this);
		interactionHint->setObjectName("interactionHint");
		interactionHint->setWordWrap(true);
		ui->verticalLayout->insertWidget(0, interactionHint);
	}

	ui->preview->setMouseTracking(true);
	ui->preview->setFocusPolicy(Qt::StrongFocus);
	ui->preview->installEventFilter(eventFilter.get());

	if (cx > 400 && cy > 400)
		resize(cx, cy);

	const char *name = obs_source_get_name(source);
	setWindowTitle(QTStr("Basic.InteractionWindow").arg(QT_UTF8(name)));

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(ui->preview->GetDisplay(), OBSBasicInteraction::DrawPreview, this);
	};

	connect(ui->preview, &OBSQTDisplay::DisplayCreated, this, addDrawCallback);
}

OBSBasicInteraction::~OBSBasicInteraction()
{
	// since QT fakes a mouse movement while destructing a widget
	// remove our event filter
	ui->preview->removeEventFilter(eventFilter.get());
}

OBSEventFilter *OBSBasicInteraction::BuildEventFilter()
{
	return new OBSEventFilter([this](QObject *, QEvent *event) {
		switch (event->type()) {
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonRelease:
		case QEvent::MouseButtonDblClick:
			return this->HandleMouseClickEvent(static_cast<QMouseEvent *>(event));
		case QEvent::MouseMove:
		case QEvent::Enter:
		case QEvent::Leave:
			return this->HandleMouseMoveEvent(static_cast<QMouseEvent *>(event));

		case QEvent::Wheel:
			return this->HandleMouseWheelEvent(static_cast<QWheelEvent *>(event));
		case QEvent::FocusIn:
		case QEvent::FocusOut:
			return this->HandleFocusEvent(static_cast<QFocusEvent *>(event));
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
			return this->HandleKeyEvent(static_cast<QKeyEvent *>(event));
		default:
			return false;
		}
	});
}

void OBSBasicInteraction::SourceRemoved(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<OBSBasicInteraction *>(data), "close");
}

void OBSBasicInteraction::SourceRenamed(void *data, calldata_t *params)
{
	const char *name = calldata_string(params, "new_name");
	QString title = QTStr("Basic.InteractionWindow").arg(QT_UTF8(name));

	QMetaObject::invokeMethod(static_cast<OBSBasicProperties *>(data), "setWindowTitle", Q_ARG(QString, title));
}

void OBSBasicInteraction::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	OBSBasicInteraction *window = static_cast<OBSBasicInteraction *>(data);

	if (!window->source)
		return;

	uint32_t sourceCX = max(obs_source_get_width(window->source), 1u);
	uint32_t sourceCY = max(obs_source_get_height(window->source), 1u);

	int x, y;
	int newCX, newCY;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);

	newCX = int(scale * float(sourceCX));
	newCY = int(scale * float(sourceCY));

	gs_viewport_push();
	gs_projection_push();
	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCX, newCY);
	obs_source_video_render(window->source);
	DrawScene3DGizmo(window->source, x, y, newCX, newCY);

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}

void OBSBasicInteraction::closeEvent(QCloseEvent *event)
{
	QDialog::closeEvent(event);
	if (!event->isAccepted())
		return;

	config_set_int(App()->GetAppConfig(), "InteractionWindow", "cx", width());
	config_set_int(App()->GetAppConfig(), "InteractionWindow", "cy", height());

	obs_display_remove_draw_callback(ui->preview->GetDisplay(), OBSBasicInteraction::DrawPreview, this);
}

bool OBSBasicInteraction::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG &msg = *static_cast<MSG *>(message);
	switch (msg.message) {
	case WM_MOVE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnMove();
		}
		break;
	case WM_DISPLAYCHANGE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnDisplayChange();
		}
	}
#else
	UNUSED_PARAMETER(message);
#endif

	return false;
}

static int TranslateQtKeyboardEventModifiers(QInputEvent *event, bool mouseEvent)
{
	int obsModifiers = INTERACT_NONE;

	if (event->modifiers().testFlag(Qt::ShiftModifier))
		obsModifiers |= INTERACT_SHIFT_KEY;
	if (event->modifiers().testFlag(Qt::AltModifier))
		obsModifiers |= INTERACT_ALT_KEY;
#ifdef __APPLE__
	// Mac: Meta = Control, Control = Command
	if (event->modifiers().testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_COMMAND_KEY;
	if (event->modifiers().testFlag(Qt::MetaModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#else
	// Handle windows key? Can a browser even trap that key?
	if (event->modifiers().testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#endif

	if (!mouseEvent) {
		if (event->modifiers().testFlag(Qt::KeypadModifier))
			obsModifiers |= INTERACT_IS_KEY_PAD;
	}

	return obsModifiers;
}

static int TranslateQtMouseEventModifiers(QMouseEvent *event)
{
	int modifiers = TranslateQtKeyboardEventModifiers(event, true);

	if (event->buttons().testFlag(Qt::LeftButton))
		modifiers |= INTERACT_MOUSE_LEFT;
	if (event->buttons().testFlag(Qt::MiddleButton))
		modifiers |= INTERACT_MOUSE_MIDDLE;
	if (event->buttons().testFlag(Qt::RightButton))
		modifiers |= INTERACT_MOUSE_RIGHT;

	return modifiers;
}

bool OBSBasicInteraction::GetSourceRelativeXY(int mouseX, int mouseY, int &relX, int &relY)
{
	float pixelRatio = devicePixelRatioF();
	int mouseXscaled = (int)roundf(mouseX * pixelRatio);
	int mouseYscaled = (int)roundf(mouseY * pixelRatio);

	QSize size = GetPixelSize(ui->preview);

	uint32_t sourceCX = max(obs_source_get_width(source), 1u);
	uint32_t sourceCY = max(obs_source_get_height(source), 1u);

	int x, y;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, size.width(), size.height(), x, y, scale);

	if (x > 0) {
		relX = int(float(mouseXscaled - x) / scale);
		relY = int(float(mouseYscaled / scale));
	} else {
		relX = int(float(mouseXscaled / scale));
		relY = int(float(mouseYscaled - y) / scale);
	}

	// Confirm mouse is inside the source
	if (relX < 0 || relX > int(sourceCX))
		return false;
	if (relY < 0 || relY > int(sourceCY))
		return false;

	return true;
}

bool OBSBasicInteraction::HandleMouseClickEvent(QMouseEvent *event)
{
	bool mouseUp = event->type() == QEvent::MouseButtonRelease;
	int clickCount = 1;
	if (event->type() == QEvent::MouseButtonDblClick)
		clickCount = 2;

	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);

	int32_t button = 0;

	switch (event->button()) {
	case Qt::LeftButton:
		button = MOUSE_LEFT;
		break;
	case Qt::MiddleButton:
		button = MOUSE_MIDDLE;
		break;
	case Qt::RightButton:
		button = MOUSE_RIGHT;
		break;
	default:
		blog(LOG_WARNING, "unknown button type %d", event->button());
		return false;
	}

	// Why doesn't this work?
	//if (event->flags().testFlag(Qt::MouseEventCreatedDoubleClick))
	//	clickCount = 2;

	QPoint pos = event->pos();
	bool insideSource = GetSourceRelativeXY(pos.x(), pos.y(), mouseEvent.x, mouseEvent.y);

	if (mouseUp || insideSource)
		obs_source_send_mouse_click(source, &mouseEvent, button, mouseUp, clickCount);

	return true;
}

bool OBSBasicInteraction::HandleMouseMoveEvent(QMouseEvent *event)
{
	struct obs_mouse_event mouseEvent = {};

	bool mouseLeave = event->type() == QEvent::Leave;

	if (!mouseLeave) {
		mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);
		QPoint pos = event->pos();
		mouseLeave = !GetSourceRelativeXY(pos.x(), pos.y(), mouseEvent.x, mouseEvent.y);
	}

	obs_source_send_mouse_move(source, &mouseEvent, mouseLeave);

	return true;
}

bool OBSBasicInteraction::HandleMouseWheelEvent(QWheelEvent *event)
{
	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtKeyboardEventModifiers(event, true);

	int xDelta = 0;
	int yDelta = 0;

	const QPoint angleDelta = event->angleDelta();
	if (!event->pixelDelta().isNull()) {
		if (angleDelta.x())
			xDelta = event->pixelDelta().x();
		else
			yDelta = event->pixelDelta().y();
	} else {
		if (angleDelta.x())
			xDelta = angleDelta.x();
		else
			yDelta = angleDelta.y();
	}

	const QPointF position = event->position();
	const int x = position.x();
	const int y = position.y();

	if (GetSourceRelativeXY(x, y, mouseEvent.x, mouseEvent.y)) {
		obs_source_send_mouse_wheel(source, &mouseEvent, xDelta, yDelta);
	}

	return true;
}

bool OBSBasicInteraction::HandleFocusEvent(QFocusEvent *event)
{
	bool focus = event->type() == QEvent::FocusIn;

	obs_source_send_focus(source, focus);

	return true;
}

bool OBSBasicInteraction::HandleKeyEvent(QKeyEvent *event)
{
	struct obs_key_event keyEvent;

	QByteArray text = event->text().toUtf8();
	keyEvent.modifiers = TranslateQtKeyboardEventModifiers(event, false);
	keyEvent.text = text.data();
	keyEvent.native_modifiers = event->nativeModifiers();
	keyEvent.native_scancode = event->nativeScanCode();
	keyEvent.native_vkey = event->nativeVirtualKey();

	bool keyUp = event->type() == QEvent::KeyRelease;

	obs_source_send_key_click(source, &keyEvent, keyUp);

	return true;
}

void OBSBasicInteraction::Init()
{
	show();
}
