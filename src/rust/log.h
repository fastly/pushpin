#ifndef RUST_LOG_H
#define RUST_LOG_H

extern "C"
{
	void log_init();
	void log_set_level(int level);
}

#endif
