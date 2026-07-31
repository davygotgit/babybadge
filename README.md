# babybadge
Baby Gender Reveal Badge

## Overview

This project was used at a friend's baby gender reveal party. The idea was to wear the an electronic badge that played teaser videos until a button on the badge is pressed to reveal the gender of the baby. 

See [Hackster](https://www.hackster.io/david-strong/baby-gender-reveal-badge-955c8a) for more high level details on the project and a demo video.

## How to build

The repository contains two Arduino IDE sketches, and some demo videos in MJPEG format. There is a server and a client sketch. The server sketch is intended to be used with an M5Stack Atom S3R with an Atomic TF Card Reader. The client sketch is intended to be used with an M5Stack Stopwatch. 

M5Stack have a great getting started guide, for Arduino IDE, here https://docs.m5stack.com/en/arduino/arduino_ide. 

To build:

1. Start the Arduino IDE and install the Async TCP library by ESP32Async.
2. Flash the Atom S3R with the src/server sketch.
3. Format an SD Card for FAT32. Use the largest allocation block size the card will allow, as this improves read peformance.
4. Copy all the *.MJP files from the videos folder to the SD Card.
5. Insert the SD Card into the Atomic TF Card Reader.
6. Connect the Atom S3R and the Atomic TF Card Reader.
7. Flash the Stopwatch with the src/client sketch.
8. Power on the Atom S3R.
9. Power on the Stopwatch. The Stopwatch will display a 5 second countdown, when it is connected to the server.

