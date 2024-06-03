#ifndef RUST_BACKTRACE_H
#define RUST_BACKTRACE_H

extern "C"
{
	void backtrace_setup_signal_handlers();
}

#endif
