# W5100 Ethernet example: DHCP address, then pings the gateway.
#   idf.py set-target esp32
#   idf.py menuconfig        (Example configuration -> pins)
#   idf.py -p PORT flash monitor
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ethernet_basic)
