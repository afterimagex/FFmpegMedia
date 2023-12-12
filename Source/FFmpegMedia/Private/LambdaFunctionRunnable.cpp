// Fill out your copyright notice in the Description page of Project Settings.


#include "LambdaFunctionRunnable.h"

/**
* @brief ����һ���߳�
* @param threadName �߳�����
* @param threadFunc �߳�ִ�з���
* @return
*/
FRunnableThread* LambdaFunctionRunnable::RunThreaded(FString threadName, std::function<void()> threadFunc)
{
	static int currentThread = 0;
	LambdaFunctionRunnable* runnable = new LambdaFunctionRunnable(threadFunc); //�����Զ����̶߳���
	FString _threadName = threadName + FString::FromInt(currentThread++); //�����߳�����
	runnable->thread = FRunnableThread::Create(runnable, *_threadName); //�����̣߳�����������
	return  runnable->thread;
}

/**
 * @brief ������
 * @param threadFunc �߳�ִ�з���
*/
LambdaFunctionRunnable::LambdaFunctionRunnable(std::function<void()> threadFunc) {
	_threadFunc = threadFunc;
}

/**
* @brief ִ���߳�
* @return
*/
uint32 LambdaFunctionRunnable::Run() {
	_threadFunc();
	return 0;
}

/**
* @brief �˳��߳�
*/
void LambdaFunctionRunnable::Exit() {
	delete this;
}
