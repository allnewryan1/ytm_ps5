/* Import stubs for the music core libraries, so the module converter can bind them. Only the
 * names matter: each becomes an import by NID. Built per library with -DLIB_<name>. */
#define F(n) void n(void) {}
#if defined(LIB_MusicCoreInterface)
F(sceMusicCoreIfInitializeInterface) F(sceMusicCoreIfSetFunctionTable) F(sceMusicCoreIfMainLoop)
#elif defined(LIB_CustomMusicAudioOut)
F(sceCustomMusicAudioOutInitialize) F(sceCustomMusicAudioOutOutput)
F(sceCustomMusicAudioOutSetVolume) F(sceCustomMusicAudioOutGetVolume)
F(sceCustomMusicAudioOutSetMuted) F(sceCustomMusicAudioOutGetMuted)
#elif defined(LIB_CustomMusicSysCallWrapper)
F(sceCustomMusicPthreadCreate) F(sceCustomMusicPthreadAttrInit) F(sceCustomMusicPthreadAttrDestroy)
F(sceCustomMusicPthreadAttrSetstacksize) F(sceCustomMusicPthreadJoin) F(sceCustomMusicKernelNanosleep)
F(sceCustomMusicKernelGettimeofday) F(sceCustomMusicNetSocket) F(sceCustomMusicNetConnect)
F(sceCustomMusicNetBind) F(sceCustomMusicNetListen) F(sceCustomMusicNetAccept)
F(sceCustomMusicNetSend) F(sceCustomMusicNetRecv) F(sceCustomMusicNetSocketClose)
F(sceCustomMusicNetSetsockopt) F(sceCustomMusicNetShutdown)
#endif
