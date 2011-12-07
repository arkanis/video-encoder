#
# A small video viewer (without sound)
#

av_view: av_view.c
	gcc av_view.c -lavformat -lavcodec -lswscale -lSDL -o av_view


#
#  A small sound player (without video) using pulse audio for output
#

av_listen: av_listen.c
	gcc -g av_listen.c -lavformat -lavcodec -lpulse-simple -o av_listen


#
# Targets for the ASF test stuff. Get libasf from google code, compile it
# into an object file and put the header file into the main dir. The library
# is then used and compiled into the ASF test programs.
#

asf_reader: asf_reader.c libasf.o
	gcc asf_reader.c libasf.o -o asf_reader

libasf.o: libasf
	gcc -c -combine libasf/src/*.c -o libasf.o -ansi -pedantic -Wall
	cp libasf/src/asf.h asf.h

libasf:
	svn export -r 107 http://libasf.googlecode.com/svn/trunk/ libasf