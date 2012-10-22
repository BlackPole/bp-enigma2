#include <lib/base/filepush.h>
#include <lib/base/eerror.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define PVR_COMMIT 1

//#define SHOW_WRITE_TIME 1

eFilePushThread::eFilePushThread(int io_prio_class, int io_prio_level, int blocksize, size_t buffersize)
	:prio_class(io_prio_class),
	 prio(io_prio_level),
	 m_sg(NULL),
	 m_stop(0),
	 m_send_pvr_commit(0),
	 m_stream_mode(0),
	 m_blocksize(blocksize),
	 m_buffersize(buffersize),
	 m_buffer((unsigned char*) mmap(NULL, buffersize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, /*ignored*/-1, 0)),
	 m_messagepump(eApp, 0)
{
	if (m_buffer == MAP_FAILED)
		eFatal("Failed to allocate filepush buffer, contact MiLo\n");
	CONNECT(m_messagepump.recv_msg, eFilePushThread::recvEvent);
}

eFilePushThread::~eFilePushThread()
{
	munmap(m_buffer, m_buffersize);
}

static void signal_handler(int x)
{
}

void eFilePushThread::thread()
{
	int eofcount = 0;
	setIoPrio(prio_class, prio);
	int buf_end = 0;

	size_t bytes_read = 0;
	off_t current_span_offset = 0;
	size_t current_span_remaining = 0;
	eDebug("FILEPUSH THREAD START");
	
		/* we set the signal to not restart syscalls, so we can detect our signal. */
	struct sigaction act;
	act.sa_handler = signal_handler; // no, SIG_IGN doesn't do it. we want to receive the -EINTR
	act.sa_flags = 0;
	sigaction(SIGUSR1, &act, 0);
	
	hasStarted();

	while (!m_stop)
	{
		if (m_sg && !current_span_remaining)
		{
			m_sg->getNextSourceSpan(m_current_position, bytes_read, current_span_offset, current_span_remaining);
			ASSERT(!(current_span_remaining % m_blocksize));
			m_current_position = current_span_offset;
			bytes_read = 0;
		}
		
		size_t maxread = m_buffersize;
		
			/* if we have a source span, don't read past the end */
		if (m_sg && maxread > current_span_remaining)
			maxread = current_span_remaining;

			/* align to blocksize */
		maxread -= maxread % m_blocksize;

		if (maxread)
			buf_end = m_source->read(m_current_position, m_buffer, maxread);
		else
			buf_end = 0;

		if (buf_end < 0)
		{
			buf_end = 0;
			/* Check m_stop after interrupted syscall. */
			if (m_stop) {
				break;
			}
			if (errno == EINTR || errno == EBUSY || errno == EAGAIN)
				continue;
			if (errno == EOVERFLOW)
			{
				eWarning("OVERFLOW while playback?");
				continue;
			}
			eDebug("eFilePushThread *read error* (%m) - not yet handled");
		}

			/* a read might be mis-aligned in case of a short read. */
		int d = buf_end % m_blocksize;
		if (d)
			buf_end -= d;

		if (buf_end == 0)
		{
				/* on EOF, try COMMITting once. */
			if (m_send_pvr_commit)
			{
				struct pollfd pfd;
				pfd.fd = m_fd_dest;
				pfd.events = POLLIN;
				switch (poll(&pfd, 1, 250)) // wait for 250ms
				{
					case 0:
						eDebug("wait for driver eof timeout");
						continue;
					case 1:
						eDebug("wait for driver eof ok");
						break;
					default:
						eDebug("wait for driver eof aborted by signal");
						/* Check m_stop after interrupted syscall. */
						if (m_stop)
							break;
						continue;
				}
			}
			
				/* in stream_mode, we are sending EOF events 
				   over and over until somebody responds.
				   
				   in stream_mode, think of evtEOF as "buffer underrun occured". */
			sendEvent(evtEOF);

			if (m_stop)
				break;
			if (m_stream_mode)
			{
				eDebug("reached EOF, but we are in stream mode. delaying 1 second.");
				sleep(1);
				continue;
			}
			else if (++eofcount < 10)
			{
				eDebug("reached EOF, but the file may grow. delaying 1 second.");
				sleep(1);
				continue;
			}
			break;
		} else
		{
			/* Write data to mux */
			int buf_start = 0;
			filterRecordData(m_buffer, buf_end);
			while ((buf_start != buf_end) && !m_stop)
			{
				int w = write(m_fd_dest, m_buffer + buf_start, buf_end - buf_start);

				if (w <= 0)
				{
					/* Check m_stop after interrupted syscall. */
					if (m_stop) {
						w = 0;
						buf_start = 0;
						buf_end = 0;
						break;
					}
					if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EBUSY))
						continue;
					eDebug("eFilePushThread WRITE ERROR");
					sendEvent(evtWriteError);
					break;
				}
				buf_start += w;
			}

			eofcount = 0;
			m_current_position += buf_end;
			bytes_read += buf_end;
			if (m_sg)
				current_span_remaining -= buf_end;
		}
	}
	sendEvent(evtStopped);
	eDebug("FILEPUSH THREAD STOP");
}

void eFilePushThread::start(ePtr<iTsSource> &source, int fd_dest)
{
	m_source = source;
	m_fd_dest = fd_dest;
	m_current_position = 0;
	resume();
}

void eFilePushThread::stop()
{
		/* if we aren't running, don't bother stopping. */
	if (!sync())
		return;

	m_stop = 1;

	eDebug("eFilePushThread stopping thread");
	sendSignal(SIGUSR1);
	kill(0);
}

void eFilePushThread::pause()
{
	stop();
}

void eFilePushThread::resume()
{
	m_stop = 0;
	run();
}

void eFilePushThread::enablePVRCommit(int s)
{
	m_send_pvr_commit = s;
}

void eFilePushThread::setStreamMode(int s)
{
	m_stream_mode = s;
}

void eFilePushThread::setScatterGather(iFilePushScatterGather *sg)
{
	m_sg = sg;
}

void eFilePushThread::sendEvent(int evt)
{
	m_messagepump.send(evt);
}

void eFilePushThread::recvEvent(const int &evt)
{
	m_event(evt);
}

void eFilePushThread::filterRecordData(const unsigned char *data, int len)
{
}




eFilePushThreadRecorder::eFilePushThreadRecorder(unsigned char* buffer, size_t buffersize):
	m_fd_source(-1),
	m_buffersize(buffersize),
	m_buffer(buffer),
	m_overflow_count(0),
	m_stop(0),
	m_messagepump(eApp, 0)
{
	CONNECT(m_messagepump.recv_msg, eFilePushThreadRecorder::recvEvent);
}

void eFilePushThreadRecorder::thread()
{
	setIoPrio(IOPRIO_CLASS_RT, 7);

	eDebug("[eFilePushThreadRecorder] THREAD START");

	/* we set the signal to not restart syscalls, so we can detect our signal. */
	struct sigaction act;
	act.sa_handler = signal_handler; // no, SIG_IGN doesn't do it. we want to receive the -EINTR
	act.sa_flags = 0;
	sigaction(SIGUSR1, &act, 0);

	hasStarted();

	/* m_stop must be evaluated after each syscall. */
	while (!m_stop)
	{
		ssize_t bytes = ::read(m_fd_source, m_buffer, m_buffersize);
		if (bytes < 0)
		{
			bytes = 0;
			if (errno == EINTR || errno == EBUSY || errno == EAGAIN)
				continue;
			if (errno == EOVERFLOW)
			{
				eWarning("[eFilePushThreadRecorder] OVERFLOW while recording");
				++m_overflow_count;
				continue;
			}
			eDebug("[eFilePushThreadRecorder] *read error* (%m) - aborting thread because i don't know what else to do.");
			sendEvent(evtReadError);
			break;
		}

#ifdef SHOW_WRITE_TIME
		struct timeval starttime;
		struct timeval now;
		gettimeofday(&starttime, NULL);
#endif
		int w = writeData(bytes);
#ifdef SHOW_WRITE_TIME
		gettimeofday(&now, NULL);
		suseconds_t diff = (1000000 * (now.tv_sec - starttime.tv_sec)) + now.tv_usec - starttime.tv_usec;
		eDebug("[eFilePushThreadRecorder] write %d bytes time: %9u us", bytes, (unsigned int)diff);
#endif
		if (w < 0)
		{
			eDebug("[eFilePushThreadRecorder] WRITE ERROR, aborting thread");
			sendEvent(evtWriteError);
			break;
		}
	}
	flush();
	sendEvent(evtStopped);
	eDebug("[eFilePushThreadRecorder] THREAD STOP");
}

void eFilePushThreadRecorder::start(int fd)
{
	m_fd_source = fd;
	m_stop = 0;
	run();
}

void eFilePushThreadRecorder::stop()
{
	/* if we aren't running, don't bother stopping. */
	if (!sync())
		return;
	m_stop = 1;
	eDebug("[eFilePushThreadRecorder] stopping thread."); /* just do it ONCE. it won't help to do this more than once. */
	sendSignal(SIGUSR1);
	kill(0);
}

void eFilePushThreadRecorder::sendEvent(int evt)
{
	m_messagepump.send(evt);
}

void eFilePushThreadRecorder::recvEvent(const int &evt)
{
	m_event(evt);
}
