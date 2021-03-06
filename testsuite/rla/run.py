#!/usr/bin/env python

imagedir = parent + "/oiio-images"
files = [ "ginsu_a_nc10.rla", "ginsu_a_ncf.rla", "ginsu_rgba_nc8.rla",
          "ginsu_rgb_nc16.rla", "imgmake_rgba_nc10.rla", "ginsu_a_nc16.rla",
          "ginsu_rgba_nc10.rla", "ginsu_rgba_ncf.rla", "ginsu_rgb_nc8.rla",
          "imgmake_rgba_nc16.rla", "ginsu_a_nc8.rla", "ginsu_rgba_nc16.rla",
          "ginsu_rgb_nc10.rla", "ginsu_rgb_ncf.rla", "imgmake_rgba_nc8.rla" ]
for f in files:
    command += rw_command (imagedir, f)

# Regression test to ensure crops work
command += oiiotool (imagedir+"/ginsu_rgb_nc8.rla -crop 100x100+100+100 -o rlacrop.rla")

outputs = [ "rlacrop.rla" ]
