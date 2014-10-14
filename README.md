ESPTempLogger
=============

Basic ESP8266 + Attiny85 + DHT22 Temperature and Humidity logger.
I've made this to experiment a bit with the ESP8266 wifi module, and hopefully make a remote sensor that posts the data to my server.

I wrote the code to work with both an atmega328 and a attiny85. The mcu sleeps in between readings, consuming about 35uA while powered down (the ESP module, even when chip-disabled, is responsable for about 80% of that).

The UDP messages are very specific to my use case; I have a node-red UDP listener that receives JSON strings from the logger and forward them as MQTT messages. But it should be very easy to change it to send whatever you might need.

Make sure your ESP8266 is using firmware version 0.922, and it's set to use 9600 baud. 
Also if you're using the arduino-tiny core, you'll run into problems with a not fully implemented Stream class (and a missing .find() function). To fix this, I've copied the Stream.h and Stream.c files from *..\Arduino\hardware\arduino\cores\arduino* to *..\Arduino\Hardware\tiny\cores\tiny* replacing what's already there.

It's way easier to tweak the code using a full arduino, then later compiling it for the tiny85 (note that there isn't a lot of space left).

To see the debug messages, make sure to uncomment the line `//#define DEBUG`

I've used this simple schematic:

![alt text](https://raw.githubusercontent.com/guibom/ESPTempLogger/master/schematic.png "Schematic")

Another similar project that posts temperature to ThingSpeak can be found at [http://www.instructables.com/id/ESP8266-Wifi-Temperature-Logger/]. I've used this project as a reference for mine.