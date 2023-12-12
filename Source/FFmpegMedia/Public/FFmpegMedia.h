// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "IMediaModule.h"
#include "IMediaOptions.h"
#include "IMediaPlayerFactory.h"
#include "Logging/LogMacros.h"

/** Log category for the FFmpegMedia module. */
DECLARE_LOG_CATEGORY_EXTERN(LogFFmpegMedia, Verbose, All);

/**
 * Interface for the FFmpegMedia module.
 */
class IFFmpegMediaModule
	: public IModuleInterface
{
public:
	
	/**
	 * @brief ����������ʵ��
	 * @param EventSink 
	 * @return 
	*/
	virtual TSharedPtr<IMediaPlayer, ESPMode::ThreadSafe> CreatePlayer(IMediaEventSink& EventSink) = 0;

	/**
	 * @brief ��ȡ������֧�ֵ��ļ���׺�б�
	 * @return 
	*/
	virtual TArray<FString> GetSupportedFileExtensions() = 0;

	/**
	 * @brief ��ȡ������֧�ֵ�uri����
	 * @return 
	*/
	virtual TArray<FString> GetSupportedUriSchemes() = 0;

public:

	/** Virtual destructor. */
	virtual ~IFFmpegMediaModule() { }
};


