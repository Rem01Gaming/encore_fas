// SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note

#ifndef _UAPI_ENCORE_FAS_H
#define _UAPI_ENCORE_FAS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FAS_ABI_VERSION 2

#define FAS_MAX_PATH_LEN 256
#define FAS_MAX_TARGETS 8
#define FAS_MAX_LISTENERS 16

/* Configuration flags. */
#define FAS_CFG_LOCK_DOWN (1u << 0)

/* Flags of struct fas_state. */
#define FAS_STATE_ACQUIRING (1u << 0)
#define FAS_STATE_DEGRADED (1u << 1)
#define FAS_STATE_PAUSED (1u << 2)

/* Flags of struct fas_event. */
#define FAS_EVF_WATCHDOG (1u << 0)

/**
 * @brief Module version.
 */
struct fas_version {
	// Number of git commits at build time.
	__u32 version;
	// Layout version of this header. It must equal to FAS_ABI_VERSION.
	__u32 abi;
	// Frequency of the arm64 system counter.
	__u32 counter_hz;
	// Reserved for future expansion.
	__u32 reserved;
};

/**
 * @brief Frame rate targets of one listener.
 */
struct fas_config {
	// FAS_CFG_* flags.
	__u32 flags;
	// Vsync period of the display.
	__u32 vsync_ns;
	// Number of valid entries in @fps. The range is 1 to FAS_MAX_TARGETS.
	__u32 count;

	// Reserved for future expansion.
	__u32 reserved;

	// Legal frame rates.
	__u32 fps[FAS_MAX_TARGETS];
};

/**
 * @brief Argument of FAS_IOC_REGISTER.
 */
struct fas_register_args {
	// Process ID of the game.
	__s32 pid;
	// The ID of the new listener.
	__s32 ctx_id;
	// File offset of the probed function inside @path.
	__u64 offset;
	// Initial targets.
	struct fas_config cfg;
	// Path of the file that holds the probed function.
	char path[FAS_MAX_PATH_LEN];
};

struct fas_remove_args {
	__s32 ctx_id;
};

struct fas_config_args {
	__s32 ctx_id;
	__u32 reserved;
	struct fas_config cfg;
};

/**
 * @brief Argument of FAS_IOC_GET_STATE.
 */
struct fas_state {
	// The listener that made the event.
	__s32 ctx_id;
	// Active target.
	__u32 fps;
	// FAS_STATE_* flags.
	__u32 flags;
	// Deficit accumulator.
	__u32 pressure_q16;
	// Sequence number of the last event of this listener.
	__u32 seq;
	// Events that the module dropped because the queue was full.
	__u32 dropped;

	// Reserved for future expansion.
	__u32 reserved[2];
};

struct fas_listener_info {
	__s32 ctx_id;
	__s32 pid;
};

struct fas_listener_list {
	__u32 count;
	__u32 reserved;
	struct fas_listener_info listeners[FAS_MAX_LISTENERS];
};

enum fas_event_type {
	FAS_EVENT_NONE = 0,
	FAS_EVENT_SMALL_JANK = 1,
	FAS_EVENT_BIG_JANK = 2,
	FAS_EVENT_BOOST_SOFT = 3,
	FAS_EVENT_BOOST_HARD = 4,
	FAS_EVENT_DEGRADED = 5,
	FAS_EVENT_RECOVERED = 6,
	FAS_EVENT_PAUSED = 7,
	FAS_EVENT_RESUMED = 8,
	FAS_EVENT_RATE_SWITCH = 9,
};

/**
 * @brief One record that read() returns.
 */
struct fas_event {
	// The listener that made the event.
	__s32 ctx_id;
	// One of enum fas_event_type.
	__u32 type;
	// Event time. The clock is CLOCK_MONOTONIC.
	__u64 timestamp_ns;
	// The interval that caused the event.
	__u64 frametime_ns;
	// Active target when the module made the event.
	__u32 fps;
	// Missed frame slots. Only jank events set it.
	__u32 missed;
	// FAS_EVF_* flags.
	__u32 flags;
	// Deficit accumulator.
	__u32 pressure_q16;
	// Counter of the listener.
	__u32 seq;
	// Reserved for future expansion.
	__u32 reserved;
} __attribute__((aligned(8)));

#define FAS_IOC_MAGIC 'F'

#define FAS_IOC_GET_VERSION _IOR(FAS_IOC_MAGIC, 0, struct fas_version)
#define FAS_IOC_REGISTER _IOWR(FAS_IOC_MAGIC, 1, struct fas_register_args)
#define FAS_IOC_REMOVE _IOW(FAS_IOC_MAGIC, 2, struct fas_remove_args)
#define FAS_IOC_SET_CONFIG _IOW(FAS_IOC_MAGIC, 3, struct fas_config_args)
#define FAS_IOC_GET_STATE _IOWR(FAS_IOC_MAGIC, 4, struct fas_state)
#define FAS_IOC_LIST _IOR(FAS_IOC_MAGIC, 5, struct fas_listener_list)

#define FAS_IOC_MAXNR 5

#endif
