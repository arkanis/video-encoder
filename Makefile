#
# A small encoder program that outputs an MP4 file. libstdc++ (the C++
# runtime library) is linked in for libmp4v2.
#

av_encode: av_encode.c libmp4v2.a
	gcc --std=c99 -I libmp4v2/include av_encode.c libmp4v2.a -lavformat -lavcodec -lswscale -lavfilter -lstdc++ -o av_encode

# The `LANG=en` on the second command is a workaround for the current
# build script of libmp4v2.
libmp4v2.a: libmp4v2
	cd libmp4v2 && LANG=en autoreconf -fiv
	cd libmp4v2 && ./configure && make -j 2
	cp libmp4v2/.libs/libmp4v2.a .

libmp4v2:
	svn checkout -r 482 http://mp4v2.googlecode.com/svn/trunk/ libmp4v2