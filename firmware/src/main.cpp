/* Copyright (c) 2026 Eoin Jordan
 * SPDX-License-Identifier: Apache-2.0
 *
 * ExecuTorch on the Arducam Pico4ML-BLE (RP2040) under Zephyr.
 *
 * 1. Load the .pte embedded in flash and run the MNIST test image baked into
 *    test_input.h; compare with the PyTorch logits (SELF-TEST PASS/FAIL).
 * 2. Grab frames from the HM01B0, squash them to 28x28, classify, and show
 *    the crop, the digit and the confidence on the ST7735, plus the IMU tilt.
 * 3. Print one JSON line per inference on the USB serial console.
 */

#include <executorch/extension/data_loader/buffer_data_loader.h>
#include <executorch/runtime/core/memory_allocator.h>
#include <executorch/runtime/core/span.h>
#include <executorch/runtime/executor/memory_manager.h>
#include <executorch/runtime/executor/method.h>
#include <executorch/runtime/executor/program.h>
#include <executorch/runtime/platform/log.h>
#include <executorch/runtime/platform/runtime.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/version.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "model_pte.h"
#include "test_input.h"
#ifdef CONFIG_APP_CAMERA
#include "camera.h"
#endif
#ifdef CONFIG_APP_IMU
#include "imu.h"
#endif
#ifdef CONFIG_APP_SCREEN
#include "screen.h"
#endif
#ifdef CONFIG_APP_USB_BOOTSEL_ON_1200_BAUD
#include <pico/bootrom.h>
#endif

using executorch::aten::Tensor;
using executorch::aten::TensorImpl;
using executorch::extension::BufferDataLoader;
using executorch::runtime::Error;
using executorch::runtime::EValue;
using executorch::runtime::HierarchicalAllocator;
using executorch::runtime::MemoryAllocator;
using executorch::runtime::MemoryManager;
using executorch::runtime::Method;
using executorch::runtime::MethodMeta;
using executorch::runtime::Program;
using executorch::runtime::Result;
using executorch::runtime::Span;
using executorch::runtime::TensorInfo;

/* MNIST normalisation, same constants as train/model.py */
static constexpr float kMean = 0.1307f;
static constexpr float kStd = 0.3081f;
static constexpr int kSide = 28;
static constexpr size_t kNumClasses = 10;

#ifndef APP_METHOD_ALLOCATOR_POOL_SIZE
#define APP_METHOD_ALLOCATOR_POOL_SIZE (64 * 1024)
#endif
#ifndef APP_TEMP_ALLOCATOR_POOL_SIZE
#define APP_TEMP_ALLOCATOR_POOL_SIZE (8 * 1024)
#endif

alignas(16) static uint8_t method_pool[APP_METHOD_ALLOCATOR_POOL_SIZE];
alignas(16) static uint8_t temp_pool[APP_TEMP_ALLOCATOR_POOL_SIZE];

namespace {

struct Runner {
	BufferDataLoader loader{model_pte, model_pte_len};
	MemoryAllocator method_allocator{APP_METHOD_ALLOCATOR_POOL_SIZE, method_pool};
	MemoryAllocator temp_allocator{APP_TEMP_ALLOCATOR_POOL_SIZE, temp_pool};
	std::vector<Span<uint8_t>> planned_spans;
	Program *program = nullptr;
	Method *method = nullptr;
	float *input = nullptr;
	size_t input_numel = 0;
	Tensor *input_tensor = nullptr;

	Error init();
	Error run(const float *pixels, float *logits, uint32_t *us);
};

Error Runner::init()
{
	Result<Program> program_res = Program::load(&loader);
	if (!program_res.ok()) {
		ET_LOG(Error, "Program::load failed: 0x%x", (unsigned)program_res.error());
		return program_res.error();
	}
	program = new Program(std::move(program_res.get()));

	const char *method_name = program->get_method_name(0).get();
	Result<MethodMeta> meta = program->method_meta(method_name);
	if (!meta.ok()) {
		return meta.error();
	}

	for (size_t id = 0; id < meta->num_memory_planned_buffers(); ++id) {
		size_t size = static_cast<size_t>(meta->memory_planned_buffer_size(id).get());
		uint8_t *buf = static_cast<uint8_t *>(method_allocator.allocate(size, 16));
		if (buf == nullptr) {
			ET_LOG(Error, "planned buffer %zu (%zu B) does not fit the method pool", id, size);
			return Error::MemoryAllocationFailed;
		}
		ET_LOG(Info, "planned buffer %zu: %zu bytes", id, size);
		planned_spans.push_back({buf, size});
	}

	static HierarchicalAllocator planned_memory({planned_spans.data(), planned_spans.size()});
	static MemoryManager memory_manager(&method_allocator, &planned_memory, &temp_allocator);

	Result<Method> method_res = program->load_method(method_name, &memory_manager);
	if (!method_res.ok()) {
		ET_LOG(Error, "load_method(%s) failed: 0x%x", method_name, (unsigned)method_res.error());
		return method_res.error();
	}
	method = new Method(std::move(method_res.get()));

	Result<TensorInfo> in_meta = meta->input_tensor_meta(0);
	if (!in_meta.ok()) {
		return in_meta.error();
	}
	input_numel = in_meta->nbytes() / sizeof(float);
	input = static_cast<float *>(method_allocator.allocate(in_meta->nbytes(), 16));
	if (input == nullptr) {
		return Error::MemoryAllocationFailed;
	}
	static TensorImpl impl(
		in_meta->scalar_type(), in_meta->sizes().size(),
		const_cast<TensorImpl::SizesType *>(in_meta->sizes().data()), input,
		const_cast<TensorImpl::DimOrderType *>(in_meta->dim_order().data()));
	static Tensor tensor(&impl);
	input_tensor = &tensor;

	ET_LOG(Info, "method '%s' loaded; input numel %zu; pool used %zu / %zu bytes", method_name,
	       input_numel, method_allocator.used_size(), method_allocator.size());
	return Error::Ok;
}

Error Runner::run(const float *pixels, float *logits, uint32_t *us)
{
	std::memcpy(input, pixels, input_numel * sizeof(float));
	Error err = method->set_input(EValue(*input_tensor), 0);
	if (err != Error::Ok) {
		ET_LOG(Error, "set_input failed: 0x%x", (unsigned)err);
		return err;
	}
	uint32_t t0 = k_cycle_get_32();
	err = method->execute();
	uint32_t t1 = k_cycle_get_32();
	if (err != Error::Ok) {
		ET_LOG(Error, "execute failed: 0x%x", (unsigned)err);
		return err;
	}
	*us = k_cyc_to_us_floor32(t1 - t0);

	const EValue &out = method->get_output(0);
	if (!out.isTensor()) {
		return Error::InvalidState;
	}
	const float *data = out.toTensor().const_data_ptr<float>();
	for (size_t i = 0; i < kNumClasses; ++i) {
		logits[i] = data[i];
	}
	return Error::Ok;
}

int argmax_pct(const float *logits, int *pct)
{
	int best = 0;
	float m = logits[0];
	for (size_t i = 1; i < kNumClasses; ++i) {
		if (logits[i] > m) {
			m = logits[i];
			best = (int)i;
		}
	}
	float sum = 0.f;
	for (size_t i = 0; i < kNumClasses; ++i) {
		sum += expf(logits[i] - m);
	}
	*pct = (int)lroundf(100.f / sum);
	return best;
}

/* Undo the MNIST normalisation so the embedded test image can be shown. */
void input_to_gray(const float *in, uint8_t *gray)
{
	for (int i = 0; i < kSide * kSide; ++i) {
		float v = (in[i] * kStd + kMean) * 255.f;
		gray[i] = (uint8_t)(v < 0.f ? 0.f : v > 255.f ? 255.f : v);
	}
}

void ascii_preview(const uint8_t *gray)
{
	static const char ramp[] = " .:-=+*#%@";
	for (int y = 0; y < kSide; ++y) {
		char line[kSide + 1];
		for (int x = 0; x < kSide; ++x) {
			line[x] = ramp[gray[y * kSide + x] * 9 / 255];
		}
		line[kSide] = 0;
		printk("  %s\n", line);
	}
}

#ifdef CONFIG_APP_CAMERA
/* Centre square of the frame, box-averaged to 28x28, inverted (a dark digit
 * on light paper becomes white-on-black like MNIST), then normalised. */
void frame_to_input(const uint8_t *frame, uint8_t *gray, float *input)
{
	const int side = CAM_H < CAM_W ? CAM_H : CAM_W;
	const int box = side / kSide;
	const int x0 = (CAM_W - box * kSide) / 2;
	const int y0 = (CAM_H - box * kSide) / 2;
	for (int y = 0; y < kSide; ++y) {
		for (int x = 0; x < kSide; ++x) {
			unsigned sum = 0;
			for (int by = 0; by < box; ++by) {
				const uint8_t *row = frame + (y0 + y * box + by) * CAM_W + x0 + x * box;
				for (int bx = 0; bx < box; ++bx) {
					sum += row[bx];
				}
			}
			uint8_t v = (uint8_t)(255 - sum / (box * box));
			gray[y * kSide + x] = v;
			input[y * kSide + x] = (v / 255.f - kMean) / kStd;
		}
	}
}
#endif

#ifdef CONFIG_APP_SCREEN
void show(const uint8_t *gray, const char *status, uint16_t status_color, int digit, int pct,
	  const char *imu_line)
{
	char line[16];
	screen_clear(COLOR_BLACK);
	screen_text(2, 2, "ExecuTorch", COLOR_WHITE, COLOR_BLACK);
	screen_text(2, 12, status, status_color, COLOR_BLACK);
	if (gray != nullptr) {
		screen_gray(12, 24, gray, kSide, kSide, 2);
	}
	if (digit >= 0) {
		snprintf(line, sizeof(line), "digit %d", digit);
		screen_text(2, 86, line, COLOR_YELLOW, COLOR_BLACK);
		snprintf(line, sizeof(line), "%3d%%", pct);
		screen_text(52, 86, line, COLOR_WHITE, COLOR_BLACK);
		screen_bar(2, 97, 76, 5, pct, pct >= 80 ? COLOR_GREEN : COLOR_YELLOW);
	}
	if (imu_line != nullptr) {
		screen_text(2, 112, imu_line, COLOR_GREY, COLOR_BLACK);
	}
	screen_text(2, 150, "pico4ml zephyr", COLOR_GREY, COLOR_BLACK);
	screen_flush();
}
#else
void show(const uint8_t *, const char *, uint16_t, int, int, const char *)
{
}
#define COLOR_GREEN 0
#define COLOR_RED 0
#define COLOR_YELLOW 0
#endif

void maybe_reboot_to_bootsel(const struct device *console)
{
#ifdef CONFIG_APP_USB_BOOTSEL_ON_1200_BAUD
	uint32_t baud = 0;
	if (uart_line_ctrl_get(console, UART_LINE_CTRL_BAUD_RATE, &baud) == 0 && baud == 1200) {
		printk("1200 baud seen: rebooting into BOOTSEL\n");
		k_msleep(100);
		reset_usb_boot(0, 0);
	}
#else
	(void)console;
#endif
}

} // namespace

int main(void)
{
	executorch::runtime::runtime_init();

	const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	/* Give a terminal a moment to attach so the banner is not lost. */
	for (int i = 0; i < 30; ++i) {
		uint32_t dtr = 0;
		uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			break;
		}
		k_msleep(100);
	}

	printk("\nExecuTorch on %s (Zephyr %s)\n", CONFIG_BOARD_TARGET, KERNEL_VERSION_STRING);
	printk("model: %u bytes in flash, method pool %u B, temp pool %u B\n",
	       (unsigned)model_pte_len, (unsigned)APP_METHOD_ALLOCATOR_POOL_SIZE,
	       (unsigned)APP_TEMP_ALLOCATOR_POOL_SIZE);

#ifdef CONFIG_APP_SCREEN
	bool screen_ok = screen_init() == 0;
	if (screen_ok) {
		show(nullptr, "loading...", COLOR_YELLOW, -1, 0, nullptr);
	}
#endif

	static Runner runner;
	if (runner.init() != Error::Ok) {
		printk("FAIL: runtime init\n");
		show(nullptr, "RUNTIME FAIL", COLOR_RED, -1, 0, nullptr);
		return 1;
	}

	static uint8_t gray[kSide * kSide];
	static float input[kSide * kSide];
	float logits[kNumClasses];
	uint32_t us = 0;

	printk("\nself-test: MNIST test image, label %d\n", TEST_INPUT_LABEL);
	bool ok = runner.run(kTestInput, logits, &us) == Error::Ok;
	float diff = 0.f;
	int pct = 0;
	int pred = ok ? argmax_pct(logits, &pct) : -1;
	if (ok) {
		for (size_t i = 0; i < kNumClasses; ++i) {
			diff = fmaxf(diff, fabsf(logits[i] - kTestExpectedLogits[i]));
		}
		printk("  predicted %d (%d%%), expected %d, max|rp2040-pytorch|=%.1e, %u us\n", pred, pct,
		       TEST_INPUT_LABEL, (double)diff, us);
		ok = pred == TEST_INPUT_LABEL && diff < 1e-3f;
	}
	printk(ok ? "SELF-TEST PASS\n" : "SELF-TEST FAIL\n");
	input_to_gray(kTestInput, gray);
	show(gray, ok ? "SELF-TEST PASS" : "SELF-TEST FAIL", ok ? COLOR_GREEN : COLOR_RED, pred, pct,
	     nullptr);

	char imu_line[16] = {0};
#ifdef CONFIG_APP_IMU
	bool imu_ok = imu_init() == 0;
#endif
#ifdef CONFIG_APP_CAMERA
	bool cam_ok = camera_init() == 0;
	if (!cam_ok) {
		printk("camera: not available, staying on the self-test result\n");
	}
#endif

	uint32_t seq = 0;
	for (;;) {
		maybe_reboot_to_bootsel(console);

#ifdef CONFIG_APP_IMU
		if (imu_ok) {
			struct imu_sample s;
			if (imu_read(&s) == 0) {
				snprintf(imu_line, sizeof(imu_line), "g%+.1f%+.1f%+.1f", (double)s.ax,
					 (double)s.ay, (double)s.az);
			}
		}
#endif
#ifdef CONFIG_APP_CAMERA
		if (cam_ok) {
			const uint8_t *frame = camera_capture();
			if (frame == nullptr) {
				/* camera_capture() already said why */
			} else {
				uint8_t lo, hi, mean;
				camera_stats(frame, &lo, &hi, &mean);
				frame_to_input(frame, gray, input);
				if (runner.run(input, logits, &us) == Error::Ok) {
					pred = argmax_pct(logits, &pct);
					printk("{\"seq\":%u,\"digit\":%d,\"p\":%d,\"infer_us\":%u,\"raw\":[%u,%u,%u],"
					       "\"imu\":\"%s\"}\n",
					       seq, pred, pct, us, lo, hi, mean, imu_line);
					if (seq % 10 == 0) {
						ascii_preview(gray);
					}
					show(gray, "camera", COLOR_WHITE, pred, pct, imu_line[0] ? imu_line : nullptr);
				}
				seq++;
			}
		} else
#endif
		{
			show(gray, ok ? "SELF-TEST PASS" : "SELF-TEST FAIL", ok ? COLOR_GREEN : COLOR_RED,
			     pred, pct, imu_line[0] ? imu_line : nullptr);
		}
		k_msleep(CONFIG_APP_CAMERA_INTERVAL_MS);
	}
	return 0;
}
