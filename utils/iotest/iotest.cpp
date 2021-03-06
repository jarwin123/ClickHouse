#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#include <iostream>
#include <iomanip>
#include <vector>
#include <random>

#include <Poco/NumberParser.h>
#include <Poco/NumberFormatter.h>
#include <Poco/Exception.h>

#include <DB/Common/Exception.h>

#include <DB/Common/ThreadPool.h>
#include <DB/Common/Stopwatch.h>

#include <cstdlib>

#ifdef __APPLE__
#include <common/apple_rt.h>
#endif

using DB::throwFromErrno;


enum Mode
{
	MODE_NONE = 0,
	MODE_READ = 1,
	MODE_WRITE = 2,
	MODE_ALIGNED = 4,
	MODE_DIRECT = 8,
	MODE_SYNC = 16,
};


struct AlignedBuffer
{
	int size;
	char * data;

	AlignedBuffer(int size_)
	{
		size_t page = sysconf(_SC_PAGESIZE);
		size = size_;
		int rc = posix_memalign(reinterpret_cast<void **>(&data), page, (size + page - 1) / page * page);
		if (data == nullptr || rc != 0)
			throwFromErrno("memalign failed");
	}

	~AlignedBuffer()
	{
		free(data);
	}
};

void thread(int fd, int mode, size_t min_offset, size_t max_offset, size_t block_size, size_t count)
{
	AlignedBuffer direct_buf(block_size);
	std::vector<char> simple_buf(block_size);

	char * buf;
	if ((mode & MODE_DIRECT))
		buf = direct_buf.data;
	else
		buf = &simple_buf[0];

	std::mt19937 rng;

	timespec times;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &times);
	rng.seed(times.tv_nsec);

	for (size_t i = 0; i < count; ++i)
	{
		long rand_result1 = rng();
		long rand_result2 = rng();
		long rand_result3 = rng();

		size_t rand_result = rand_result1 ^ (rand_result2 << 22) ^ (rand_result3 << 43);
		size_t offset;
		if ((mode & MODE_DIRECT) || (mode & MODE_ALIGNED))
			offset = min_offset + rand_result % ((max_offset - min_offset) / block_size) * block_size;
		else
			offset = min_offset + rand_result % (max_offset - min_offset - block_size + 1);

		if (mode & MODE_READ)
		{
			if (static_cast<int>(block_size) != pread(fd, buf, block_size, offset))
				throwFromErrno("Cannot read");
		}
		else
		{
			if (static_cast<int>(block_size) != pwrite(fd, buf, block_size, offset))
				throwFromErrno("Cannot write");
		}
	}
}


int mainImpl(int argc, char ** argv)
{
	const char * file_name = 0;
	int mode = MODE_NONE;
	size_t min_offset = 0;
	size_t max_offset = 0;
	size_t block_size = 0;
	size_t threads = 0;
	size_t count = 0;

	if (argc != 8)
	{
		std::cerr << "Usage: " << argv[0] << " file_name (r|w)[a][d][s] min_offset max_offset block_size threads count" << std::endl <<
		             "a - aligned, d - direct, s - sync" << std::endl;
		return 1;
	}

	file_name = argv[1];
	min_offset = Poco::NumberParser::parseUnsigned64(argv[3]);
	max_offset = Poco::NumberParser::parseUnsigned64(argv[4]);
	block_size = Poco::NumberParser::parseUnsigned64(argv[5]);
	threads = Poco::NumberParser::parseUnsigned(argv[6]);
	count = Poco::NumberParser::parseUnsigned(argv[7]);

	for (int i = 0; argv[2][i]; ++i)
	{
		char c = argv[2][i];
		switch(c)
		{
			case 'r':
				mode |= MODE_READ;
				break;
			case 'w':
				mode |= MODE_WRITE;
				break;
			case 'a':
				mode |= MODE_ALIGNED;
				break;
			case 'd':
				mode |= MODE_DIRECT;
				break;
			case 's':
				mode |= MODE_SYNC;
				break;
			default:
				throw Poco::Exception("Invalid mode");
		}
	}

	ThreadPool pool(threads);

	#ifndef __APPLE__
	int fd = open(file_name, ((mode & MODE_READ) ? O_RDONLY : O_WRONLY) | ((mode & MODE_DIRECT) ? O_DIRECT : 0) | ((mode & MODE_SYNC) ? O_SYNC : 0));
	#else
	int fd = open(file_name, ((mode & MODE_READ) ? O_RDONLY : O_WRONLY) | ((mode & MODE_SYNC) ? O_SYNC : 0));
	#endif
	if (-1 == fd)
		throwFromErrno("Cannot open file");
	#ifdef __APPLE__
	if (mode & MODE_DIRECT)
		if (fcntl(fd, F_NOCACHE, 1) == -1)
			throwFromErrno("Cannot open file");
	#endif
	Stopwatch watch;

	for (size_t i = 0; i < threads; ++i)
		pool.schedule(std::bind(thread, fd, mode, min_offset, max_offset, block_size, count));
	pool.wait();

	fsync(fd);

	watch.stop();

	if (0 != close(fd))
		throwFromErrno("Cannot close file");

	std::cout << std::fixed << std::setprecision(2)
		<< "Done " << count << " * " << threads << " ops";
	if (mode & MODE_ALIGNED)
		std::cout << " (aligned)";
	if (mode & MODE_DIRECT)
		std::cout << " (direct)";
	if (mode & MODE_SYNC)
		std::cout << " (sync)";
	std::cout << " in " << watch.elapsedSeconds() << " sec."
		<< ", " << count * threads / watch.elapsedSeconds() << " ops/sec."
		<< ", " << count * threads * block_size / watch.elapsedSeconds() / 1000000 << " MB/sec."
		<< std::endl;

    return 0;
}


int main(int argc, char ** argv)
{
	try
	{
		return mainImpl(argc, argv);
	}
	catch (const Poco::Exception & e)
	{
		std::cerr << e.what() << ", " << e.message() << std::endl;
		return 1;
	}
}
