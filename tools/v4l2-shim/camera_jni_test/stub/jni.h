#pragma once
typedef void *jobject;
typedef void *jclass;
typedef int jint;
typedef long long jlong;
typedef unsigned char jboolean;
struct JNINativeInterface_ {
    void *(*GetDirectBufferAddress)(void *env, jobject buf);
    jlong (*GetDirectBufferCapacity)(void *env, jobject buf);
};
typedef const struct JNINativeInterface_ *JNIEnv;
#define JNIEXPORT
#define JNICALL
