# Surround View unit test application

Demo application demonstrates Surround View on H3, H2, V2H boards.

## Build
Project uses cmake tool to build an application.

(example for H3)


```
. /opt/poky/2.0.2/environment-setup-aarch64-poky-linux 
mkdir build
cd build
cmake -DSV_TARGET_PLATFORM=GEN3 ../
make

```
Optional values for SV_TARGET_PLATFORM are GEN3, GEN2
Prebuilt Surround View libraries are stored in libs subdirectory.

## Run
Copy resources files to directory where application will be runned

To run application 
```
usage: sv-utest [options]

Available options:
	-d|--debug	- set debug level 0-6
	-f|--format      - video format, must be first in command line (available options: uyvy, nv12,i420)
	-i|--iface	- for MJPEG cameras only, network interface
	-m|--mac	- for MJPEG cameras only, cameras MAC list: mac1,mac2,mac3,mac4
	        	  where mac is in form AA:BB:CC:DD:EE:FF
	-v|--vin	- V4L2 camera devices list: cam1,cam2,cam3,cam4
	        	  where cam is in form /dev/videoX			  
	-c|--cfg	- playback tracks configuration to load
	-o|--output	- desired Weston display output number 0, 1,.., N
	-w|--js		- joystick device name
	-h|--help	- this help
	-V|--version	- print version
	--resolution	- window size as WidthxHeight
	--camres	- camera output size as WIDTHxHEIGHT

Auxiliary calibration options:
	--intrinsicframes <mask1>,<mask2>,<mask3>,<mask4> - specify comma-separated
	         list of file masks which can be loaded in calibration UI
	         in place of grabbed frames
	--extrinsicframes <mask>,<mask2>,<mask3>,<mask4> - specify file masks
	         which can be loaded in extrinsic calibration UI
	         in place of grabbed frames
	--intrinsicoutput <directory> - specify directory where grabbed
	         intrinsic calibration frames are stored
	         with camera%d_frame%d.png file names
	--extrinsicoutput <directory> - specify directory where grabbed
	         extrinsic calibration frames are stored
	         with extrinsic_frame%d.png file names
Calibration options:
	--intrinsics-cell-width <value> - width of the cell on the chess pattern board
	         in mm, default 50
	--intrinsics-cell-height <value> - height of the cell on the chess pattern board
	         in mm, default 50
	--intrinsics-board-width <value> - width of the chess pattern board
	         in terms of cross between cells, default 9
	--intrinsics-board-height <value> - height of the chess pattern board
	         in terms of cross between cells, default 6
	--intrinsics-grab-interval <value> - time interval between frame capture attempts
	         in timer mode in seconds, default 10 seconds
	--intrinsics-num-frames <value> - number of frames to grab for intrinsics calculation,
	         default 15 frames
	--extrinsics-num-circles <value> - number of circles on pattern (2 or 3)
	--extrinsics-circles-param <value> - circles pattern parameter: radius for 2-circles pattern,
	         length between circles centers for 3-circles pattern

Example:
# Cameras
sv-utest  -v /dev/video0,/dev/video1,/dev/video2,/dev/video3
# Video files
sv-utest -f nv12 -c tracks.cfg

cat tracks.cfg
[sv-track]
info=Parking without point-cloud data
file=video1.mkv,video2.mkv,video3.mkv,video4.mkv