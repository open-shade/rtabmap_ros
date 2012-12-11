/*
 * CoreWrapper.cpp
 *
 *  Created on: 2 févr. 2010
 *      Author: labm2414
 */

#include "CoreWrapper.h"
#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <cv_bridge/cv_bridge.h>
#include <rtabmap/core/RtabmapEvent.h>
#include <rtabmap/core/Rtabmap.h>
#include <rtabmap/core/Camera.h>
#include <rtabmap/core/Parameters.h>
#include <utilite/UEventsManager.h>
#include <utilite/ULogger.h>
#include <utilite/UFile.h>
#include <utilite/UStl.h>
#include <opencv2/highgui/highgui.hpp>

//msgs
#include "rtabmap/Info.h"
#include "rtabmap/InfoEx.h"

using namespace rtabmap;

CoreWrapper::CoreWrapper(bool deleteDbOnStart) :
		rtabmap_(0)
{
	ros::NodeHandle nh("~");
	infoPub_ = nh.advertise<rtabmap::Info>("info", 1);
	infoPubEx_ = nh.advertise<rtabmap::InfoEx>("infoEx", 1);
	parametersLoadedPub_ = nh.advertise<std_msgs::Empty>("parameters_loaded", 1);

	rtabmap_ = new Rtabmap();
	UEventsManager::addHandler(rtabmap_);
	loadNodeParameters(rtabmap_->getIniFilePath());

	if(deleteDbOnStart)
	{
		this->post(new RtabmapEventCmd(RtabmapEventCmd::kCmdDeleteMemory));
	}

	rtabmap_->init();

	resetMemorySrv_ = nh.advertiseService("resetMemory", &CoreWrapper::resetMemoryCallback, this);
	dumpMemorySrv_ = nh.advertiseService("dumpMemory", &CoreWrapper::dumpMemoryCallback, this);
	deleteMemorySrv_ = nh.advertiseService("deleteMemory", &CoreWrapper::deleteMemoryCallback, this);
	dumpPredictionSrv_ = nh.advertiseService("dumpPrediction", &CoreWrapper::dumpPredictionCallback, this);

	nh = ros::NodeHandle();
	parametersUpdatedTopic_ = nh.subscribe("rtabmap_gui/parameters_updated", 1, &CoreWrapper::parametersUpdatedCallback, this);

	image_transport::ImageTransport it(nh);
	imageTopic_ = it.subscribe("image", 1, &CoreWrapper::imageReceivedCallback, this);

	UEventsManager::addHandler(this);
}

CoreWrapper::~CoreWrapper()
{
	this->saveNodeParameters(rtabmap_->getIniFilePath());
	delete rtabmap_;
}

void CoreWrapper::start()
{
	rtabmap_->start();
}

void CoreWrapper::loadNodeParameters(const std::string & configFile)
{
	ROS_INFO("Loading parameters from %s", configFile.c_str());
	if(!UFile::exists(configFile.c_str()))
	{
		ROS_WARN("Config file doesn't exist!");
	}

	ParametersMap parameters = Parameters::getDefaultParameters();
	Rtabmap::readParameters(configFile.c_str(), parameters);

	ros::NodeHandle nh("~");
	for(ParametersMap::const_iterator i=parameters.begin(); i!=parameters.end(); ++i)
	{
		nh.setParam(i->first, i->second);
	}
	parametersLoadedPub_.publish(std_msgs::Empty());
}

void CoreWrapper::saveNodeParameters(const std::string & configFile)
{
	ROS_INFO("Saving parameters to %s", configFile.c_str());

	if(!UFile::exists(configFile.c_str()))
	{
		ROS_WARN("Config file doesn't exist, a new one will be created.");
	}

	ParametersMap parameters = Parameters::getDefaultParameters();
	ros::NodeHandle nh("~");
	for(ParametersMap::iterator iter=parameters.begin(); iter!=parameters.end(); ++iter)
	{
		std::string value;
		if(nh.getParam(iter->first,value))
		{
			iter->second = value;
		}
	}

	Rtabmap::writeParameters(configFile.c_str(), parameters);

	std::string databasePath = parameters.at(Parameters::kRtabmapWorkingDirectory())+"/LTM.db";
	ROS_INFO("Database/long-term memory (%lu MB) is located at %s/LTM.db", UFile::length(databasePath)/1000000, databasePath.c_str());
}

void CoreWrapper::imageReceivedCallback(const sensor_msgs::ImageConstPtr & msg)
{
	if(msg->data.size())
	{
		ROS_INFO("Received image.");
		cv_bridge::CvImageConstPtr ptr = cv_bridge::toCvShare(msg);
		UEventsManager::post(new CameraEvent(ptr->image.clone()));
	}
}

bool CoreWrapper::resetMemoryCallback(std_srvs::Empty::Request&, std_srvs::Empty::Response&)
{
	UEventsManager::post(new RtabmapEventCmd(RtabmapEventCmd::kCmdResetMemory));
	return true;
}

bool CoreWrapper::dumpMemoryCallback(std_srvs::Empty::Request&, std_srvs::Empty::Response&)
{
	UEventsManager::post(new RtabmapEventCmd(RtabmapEventCmd::kCmdDumpMemory));
	return true;
}

bool CoreWrapper::deleteMemoryCallback(std_srvs::Empty::Request&, std_srvs::Empty::Response&)
{
	UEventsManager::post(new RtabmapEventCmd(RtabmapEventCmd::kCmdDeleteMemory));
	return true;
}

bool CoreWrapper::dumpPredictionCallback(std_srvs::Empty::Request&, std_srvs::Empty::Response&)
{
	UEventsManager::post(new RtabmapEventCmd(RtabmapEventCmd::kCmdDumpPrediction));
	return true;
}

void CoreWrapper::parametersUpdatedCallback(const std_msgs::EmptyConstPtr & msg)
{
	rtabmap::ParametersMap parameters = rtabmap::Parameters::getDefaultParameters();
	ros::NodeHandle nh("~");
	for(rtabmap::ParametersMap::iterator iter=parameters.begin(); iter!=parameters.end(); ++iter)
	{
		std::string value;
		if(nh.getParam(iter->first, value))
		{
			iter->second = value;
		}
	}
	ROS_INFO("Updating parameters");
	UEventsManager::post(new ParamEvent(parameters));
}

void CoreWrapper::handleEvent(UEvent * anEvent)
{
	if(anEvent->getClassName().compare("RtabmapEvent") == 0)
	{
		if(infoPub_.getNumSubscribers() || infoPubEx_.getNumSubscribers())
		{
			RtabmapEvent * rtabmapEvent = (RtabmapEvent*)anEvent;
			const Statistics & stat = rtabmapEvent->getStats();

			if(infoPub_.getNumSubscribers())
			{
				ROS_INFO("Sending RtabmapInfo msg (last_id=%d)...", stat.refImageId());
				rtabmap::InfoPtr msg(new rtabmap::Info);
				msg->refId = stat.refImageId();
				msg->loopClosureId = stat.loopClosureId();
				infoPub_.publish(msg);
			}

			if(infoPubEx_.getNumSubscribers())
			{
				ROS_INFO("Sending infoEx msg (last_id=%d)...", stat.refImageId());
				rtabmap::InfoExPtr msg(new rtabmap::InfoEx);
				msg->refId = stat.refImageId();
				msg->loopClosureId = stat.loopClosureId();

				// Detailed info
				if(stat.extended())
				{
					if(!stat.refImage().empty())
					{
						cv_bridge::CvImage img;
						if(stat.refImage().channels() == 1)
						{
							img.encoding = sensor_msgs::image_encodings::MONO8;
						}
						else
						{
							img.encoding = sensor_msgs::image_encodings::BGR8;
						}
						img.image = stat.refImage();
						sensor_msgs::ImagePtr rosMsg = img.toImageMsg();
						rosMsg->header.frame_id = "camera";
						rosMsg->header.stamp = ros::Time::now();
						msg->refImage = *rosMsg;
					}
					if(!stat.loopImage().empty())
					{
						cv_bridge::CvImage img;
						if(stat.loopImage().channels() == 1)
						{
							img.encoding = sensor_msgs::image_encodings::MONO8;
						}
						else
						{
							img.encoding = sensor_msgs::image_encodings::BGR8;
						}
						img.image = stat.loopImage();
						sensor_msgs::ImagePtr rosMsg = img.toImageMsg();
						rosMsg->header.frame_id = "camera";
						rosMsg->header.stamp = ros::Time::now();
						msg->loopImage = *rosMsg;
					}

					//Posterior, likelihood, childCount
					msg->posteriorKeys = uKeys(stat.posterior());
					msg->posteriorValues = uValues(stat.posterior());
					msg->likelihoodKeys = uKeys(stat.likelihood());
					msg->likelihoodValues = uValues(stat.likelihood());
					msg->weightsKeys = uKeys(stat.weights());
					msg->weightsValues = uValues(stat.weights());

					//Features stuff...
					msg->refWordsKeys = uKeys(stat.refWords());
					msg->refWordsValues = std::vector<rtabmap::KeyPoint>(stat.refWords().size());
					int index = 0;
					for(std::multimap<int, cv::KeyPoint>::const_iterator i=stat.refWords().begin();
						i!=stat.refWords().end();
						++i)
					{
						msg->refWordsValues.at(index).angle = i->second.angle;
						msg->refWordsValues.at(index).response = i->second.response;
						msg->refWordsValues.at(index).ptx = i->second.pt.x;
						msg->refWordsValues.at(index).pty = i->second.pt.y;
						msg->refWordsValues.at(index).size = i->second.size;
						msg->refWordsValues.at(index).octave = i->second.octave;
						msg->refWordsValues.at(index).class_id = i->second.class_id;
						++index;
					}

					msg->loopWordsKeys = uKeys(stat.loopWords());
					msg->loopWordsValues = std::vector<rtabmap::KeyPoint>(stat.loopWords().size());
					index = 0;
					for(std::multimap<int, cv::KeyPoint>::const_iterator i=stat.loopWords().begin();
						i!=stat.loopWords().end();
						++i)
					{
						msg->loopWordsValues.at(index).angle = i->second.angle;
						msg->loopWordsValues.at(index).response = i->second.response;
						msg->loopWordsValues.at(index).ptx = i->second.pt.x;
						msg->loopWordsValues.at(index).pty = i->second.pt.y;
						msg->loopWordsValues.at(index).size = i->second.size;
						msg->loopWordsValues.at(index).octave = i->second.octave;
						msg->loopWordsValues.at(index).class_id = i->second.class_id;
						++index;
					}

					// Statistics data
					msg->statsKeys = uKeys(stat.data());
					msg->statsValues = uValues(stat.data());
				}
				infoPubEx_.publish(msg);
			}
		}
	}
}
