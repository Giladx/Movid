/***********************************************************************
 ** Copyright (C) 2010 Movid Authors.  All rights reserved.
 **
 ** This file is part of the Movid Software.
 **
 ** This file may be distributed under the terms of the Q Public License
 ** as defined by Trolltech AS of Norway and appearing in the file
 ** LICENSE included in the packaging of this file.
 **
 ** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 ** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 **
 ** Contact info@movid.org if any conditions of this licensing are
 ** not clear to you.
 **
 **********************************************************************/


#include <stdlib.h>
#include <assert.h>
#include <sstream>
#include <iostream>
#include <errno.h>

#include "pasync.h"

#include "moModule.h"
#include "moDataStream.h"
#include "moLog.h"
#include "moThread.h"
#include "moUtils.h"

LOG_DECLARE("Module");

static unsigned int idcount = 0;

// callback called when the property gui_feedback is changed.
static void module_gui_feedback_cb(moProperty *property, void *userdata) {
	assert( property != NULL );
	assert( userdata != NULL );
	moModule *module = NULL;
	std::vector<std::string> tokens;

	// split the value in 3 (type, x, y)
	std::string s = property->asString();
	if ( s == "" )
		return;

	tokens = moUtils::tokenize(s, ";");
	if ( tokens.size() != 3 )
		return;

	module = static_cast<moModule *>(userdata);
	module->guiFeedback(
		tokens[0],
		atof(tokens[1].c_str()),
		atof(tokens[2].c_str())
	);
};

moModule::moModule(unsigned int capabilities, int input_count, int output_count) {
	this->capabilities	= capabilities;
	this->input_count	= input_count;
	this->output_count	= output_count;
	this->is_started	= false;
	this->owner			= NULL;
	this->is_error		= false;
	this->error_msg		= "";
	this->thread		= NULL;
	this->use_thread	= false;
	this->need_update	= false;
	this->thread_trigger = NULL;
	this->mtx			= new pt::mutex();

	this->properties["use_thread"] = new moProperty(false);

	// create the default properties used for gui
	// the gui_feedback will be formatted as [down|move|up];x;y
	this->properties["gui_feedback"] = new moProperty("");
	this->properties["gui_feedback"]->addCallback(module_gui_feedback_cb, this);
}

moModule::~moModule() {
	this->stop();

	if ( this->output_infos.size() > 0 ) {
		std::map<int, moDataStreamInfo*>::iterator it;
		for ( it = this->output_infos.begin(); it != this->output_infos.end(); it++ )
			delete it->second;
	}

	if ( this->input_infos.size() > 0 ) {
		std::map<int, moDataStreamInfo*>::iterator it;
		for ( it = this->input_infos.begin(); it != this->input_infos.end(); it++ )
			delete it->second;
	}

	if ( this->properties.size() > 0 ) {
		std::map<std::string, moProperty*>::iterator it;
		for ( it = this->properties.begin(); it != this->properties.end(); it++ ) {
			delete (*it).second;
			(*it).second = NULL;
		}
	}

	if ( this->thread_trigger != NULL )
		delete this->thread_trigger;
	delete this->mtx;
}

std::string moModule::createId(std::string base) {
	std::ostringstream oss;
	oss << base << (idcount++);
	return oss.str();
}

unsigned int moModule::getCapabilities() {
	return this->capabilities;
}

int moModule::getInputCount() {
	return this->input_count;
}

int moModule::getOutputCount() {
	return this->output_count;
}

int moModule::getInputIndex(moDataStream *ds) {
	for(int i=0; i < this->getInputCount(); i++){
		if (ds == this->getInput(i))
			return i;
	}
	return -1;
}

int moModule::getOutputIndex(moDataStream *ds){
	for(int i=0; i < this->getOutputCount(); i++){
		if (ds == this->getOutput(i))
			return i;
	}
	return -1;
}

void moModule::notifyData(moDataStream *source) {
}

void _thread_process(moThread *thread) {
	moModule *module = (moModule *)thread->getUserData();
	while ( !thread->wantQuit() ) {
		if ( !module->needUpdate(true) )
			continue;
		module->update();
	}
}

void moModule::start() {
	this->use_thread = this->property("use_thread").asBool();
	if ( this->use_thread ) {
		if ( this->thread_trigger == NULL ) {
			LOGM(MO_TRACE) << "create trigger";
			this->thread_trigger = new pt::trigger(true, false);
		}

		LOGM(MO_TRACE) << "start thread";
		this->thread = new moThread(_thread_process, this);
		if ( this->thread == NULL ) {
			LOGM(MO_ERROR) << "unable to create thread";
			this->setError("Error while creating thread");
			this->use_thread = false;
		} else {
			this->thread->start();
		}
	}

	this->is_started = true;
	LOGM(MO_DEBUG) << "start";
}

void moModule::stop() {
	if ( this->use_thread &&  this->thread != NULL ) {
		this->thread->stop();
		this->thread_trigger->post();
		this->thread->waitfor();
		delete this->thread;
		this->thread = NULL;
		this->use_thread = false;
	}

	this->need_update = false;
	this->is_started = false;
	LOG(MO_DEBUG) << "stop <" << this->property("id").asString() << ">";
}

void moModule::lock() {
	this->mtx->lock();
}

void moModule::unlock() {
	this->mtx->unlock();
}

bool moModule::isStarted() {
	return this->is_started;
}

moDataStreamInfo *moModule::getInputInfos(int n) {
	std::map<int, moDataStreamInfo*>::iterator it;
	it = this->input_infos.find(n);
	if ( it == this->input_infos.end() )
		return NULL;
	return it->second;
}

moDataStreamInfo *moModule::getOutputInfos(int n) {
	std::map<int, moDataStreamInfo*>::iterator it;
	it = this->output_infos.find(n);
	if ( it == this->output_infos.end() )
		return NULL;
	return it->second;
}

moProperty &moModule::property(std::string str) {
	std::map<std::string, moProperty*>::iterator it;
	it = this->properties.find(str);
	if ( it == this->properties.end() ) {
		this->properties[str] = new moProperty("", "?? auto created ??");
		return *(this->properties[str]);
	}
	return *it->second;
}

std::map<std::string, moProperty*> &moModule::getProperties() {
	return this->properties;
}

void moModule::describe() {
	std::cout << "Module: " << this->getName() << std::endl;
	std::cout << "Author: " << this->getAuthor() << std::endl;
	std::cout << "Description: " << this->getDescription() << std::endl;

	std::cout << "Capabilities: ";
	if ( this->getCapabilities() & MO_MODULE_INPUT )
		std::cout << "input,";
	if ( this->getCapabilities() & MO_MODULE_OUTPUT )
		std::cout << "output,";
	if ( this->getCapabilities() & MO_MODULE_GUI )
		std::cout << "gui,";
	std::cout << std::endl;

	if ( this->properties.size() > 0 ) {
		std::cout << std::endl;
		std::cout << "Properties: " << std::endl;

		std::map<std::string, moProperty*>::iterator it;
		for ( it = this->properties.begin(); it != this->properties.end(); it++ ) {
			std::cout << " " << (*it).first << ": " \
				<< "type=" << moProperty::getPropertyTypeName((*it).second->getType()) << ", "\
				<< "default=" << (*it).second->asString() \
				<< std::endl;
		}
	}

	if ( this->getCapabilities() & MO_MODULE_INPUT ) {
		std::cout << std::endl;
		std::cout << "Input :" << std::endl;
		for ( int i = 0; i < this->getInputCount(); i++ ) {
			std::cout << " " << i << ": name=" \
				<< this->getInputInfos(i)->getName() << ", type=" \
				<< this->getInputInfos(i)->getType() << ", desc=" \
				<< this->getInputInfos(i)->getDescription() << std::endl;
		}
	}

	if ( this->getCapabilities() & MO_MODULE_OUTPUT ) {
		std::cout << std::endl;
		std::cout << "Output :" << std::endl;
		for ( int i = 0; i < this->getOutputCount(); i++ ) {
			std::cout << " " << i << ": name=" \
				<< this->getOutputInfos(i)->getName() << ", type=" \
				<< this->getOutputInfos(i)->getType() << ", desc=" \
				<< this->getOutputInfos(i)->getDescription() << std::endl;
		}
	}

	std::cout << std::endl;
}

bool moModule::isPipeline() {
	return false;
}

bool moModule::haveError() {
	return this->is_error;
}

void moModule::setError(const std::string& msg) {
	this->error_msg = msg;
	this->is_error = true;
}

std::string moModule::getLastError() {
	std::ostringstream oss;
	this->is_error = false;
	oss << "<" << this->property("id").asString() << "> " << this->error_msg;
	return oss.str();
}

void moModule::poll() {
	if ( this->use_thread )
		return;
	if ( this->needUpdate() )
		this->update();
}

void moModule::notifyUpdate() {
	this->need_update = true;
	if ( this->use_thread )
		this->thread_trigger->post();
}

bool moModule::needUpdate(bool lock) {
	if ( this->need_update ) {
		this->need_update = false;
		return true;
	} else if ( lock == false )
		return false;

	// call from a thread
	if ( lock ) {
		assert(this->thread != NULL);
		this->thread_trigger->wait();
	}

	if ( this->need_update ) {
		this->need_update = false;
		return true;
	}

	return false;
}

bool moModule::serializeCreation(std::ostringstream &oss) {
	std::string id = this->property("id").asString();

	oss << "pipeline create " << this->getName() << " " << id << std::endl;

	if ( this->properties.size() > 0 ) {
		std::map<std::string, moProperty*>::iterator it;
		for ( it = this->properties.begin(); it != this->properties.end(); it++ ) {
			oss << "pipeline set " << id << " "
				<< (*it).first << " "
				<< ((*it).second)->asString() << std::endl;
		}
	}

	oss << "" << std::endl;

	return true;
}


bool moModule::serializeConnections(std::ostringstream &oss) {
	std::string id = this->property("id").asString();

	// for every Output Connection that we have
	for (int i=0; i < this->getOutputCount(); i++) {
		moDataStream* ds = this->getOutput(i);
		if ( ds == NULL ) continue;

		for ( unsigned int j=0; j < ds->getObserverCount(); j++ ) {
			moModule* observer = ds->getObserver(j);
			oss << "pipeline connect " << id << " " << i  << " "
				<< observer->property("id").asString() << " "
				<< observer->getInputIndex(ds)<< " " << std::endl;
		}
	}

	return true;
}

//
// Feedback part between GUI and Module
//
// Gui is transmit is information with mouse (down/move/up + position)
// with the property gui_feedback.
//
// Module with MO_MODULE_GUI capability are able to send instruction on the GUI
// Theses instructions are taken with the new /pipeline/gui command
//
// Here is a list of possible instructions:
//   viewport w h;
//   color r g b;
//   text x y label;
//   style <filled|stroke>;
//   rect x y w h;
//   circle x y r;
//   line x1 y1 x2 y2;
//
//
// Theses instructions are very basic, but can be understood by any UI.
// Do not add your instruction without having the approbation of the core team.
//

void moModule::guiFeedback(const std::string& type, double x, double y) {
}

std::vector<std::string> &moModule::getGui(void) {
	assert(this->getCapabilities() & MO_MODULE_GUI);
	return this->gui;
}

