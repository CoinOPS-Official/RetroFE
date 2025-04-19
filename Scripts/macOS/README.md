These scripts allow the GStreamer.framework to be cut down after a release build has been produced. 

get_gstreamer_libs_used.sh queries a list of dylibs RetroFE has hooks in, filters that list to just GStreamer and then produces gstreamer_dylibs.txt. It should be ran while RetroFE is running in a user-like environment such as that produced by the demo environment.

delete_gstreamer__unused_libs.sh can then take the list of gstreamer_dylibs.txt once pasted into the REQUIRED_LIBRARIES variable and deletes everything in GStreamer.framework that isn't in the list. REQUIRED_LIBRARIES currently has a preset list based on the current build, this can change in the future and automation will be taken into account if neccessary.