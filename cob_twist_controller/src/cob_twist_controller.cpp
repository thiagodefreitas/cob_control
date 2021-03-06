/*!
 *****************************************************************
 * \file
 *
 * \note
 *   Copyright (c) 2014 \n
 *   Fraunhofer Institute for Manufacturing Engineering
 *   and Automation (IPA) \n\n
 *
 *****************************************************************
 *
 * \note
 *   Project name: care-o-bot
 * \note
 *   ROS stack name: cob_control
 * \note
 *   ROS package name: cob_twist_controller
 *
 * \author
 *   Author: Felix Messmer, email: Felix.Messmer@ipa.fraunhofer.de
 *
 * \date Date of creation: April, 2014
 *
 * \brief
 *   This package provides a generic Twist controller for the Care-O-bot
 *
 ****************************************************************/
#include <ros/ros.h>

#include <cob_twist_controller/cob_twist_controller.h>

#include <kdl_conversions/kdl_msg.h>
#include <tf/transform_datatypes.h>


void CobTwistController::initialize()
{
	///get params
	XmlRpc::XmlRpcValue jn_param;
	if (nh_.hasParam("joint_names"))
	{	nh_.getParam("joint_names", jn_param);	}
	else
	{	ROS_ERROR("Parameter joint_names not set");	}
	
	dof_ = jn_param.size();
	for(unsigned int i=0; i<dof_; i++)
	{	joints_.push_back((std::string)jn_param[i]);	}
	
	
	if (nh_.hasParam("base_link"))
	{
		nh_.getParam("base_link", chain_base_);
	}
	if (nh_.hasParam("tip_link"))
	{
		nh_.getParam("tip_link", chain_tip_);
	}
	
	
	///parse robot_description and generate KDL chains
	KDL::Tree my_tree;
	std::string robot_desc_string;
	nh_.param("/robot_description", robot_desc_string, std::string());
	if (!kdl_parser::treeFromString(robot_desc_string, my_tree)){
		ROS_ERROR("Failed to construct kdl tree");
		return;
	}
	my_tree.getChain(chain_base_, chain_tip_, chain_);
	if(chain_.getNrOfJoints() == 0)
	{
		ROS_ERROR("Failed to initialize kinematic chain");
		return;
	}
	
	///parse robot_description and set velocity limits
	urdf::Model model;
	if (!model.initParam("/robot_description"))
	{
		ROS_ERROR("Failed to parse urdf file");
	}
	
	ROS_INFO("Successfully parsed urdf file");
	for(unsigned int i=0; i<dof_; i++)
	{
		limits_vel_.push_back(model.getJoint(joints_[i])->limits->velocity);
	}
	
	///initialize configuration control solver
	//p_iksolver_vel_ = new KDL::ChainIkSolverVel_pinv(chain_, 0.001, 5);
	p_augmented_solver_ = new augmented_solver(chain_, 0.001, 5);
	
	///initialize ROS interfaces
	jointstate_sub = nh_.subscribe("/joint_states", 1, &CobTwistController::jointstate_cb, this);
	twist_sub = nh_.subscribe("command_twist", 1, &CobTwistController::twist_cb, this);
	vel_pub = nh_.advertise<brics_actuator::JointVelocities>("command_vel", 10);
	
	
	///initialize variables and current joint values and velocities
	last_q_ = KDL::JntArray(chain_.getNrOfJoints());
	last_q_dot_ = KDL::JntArray(chain_.getNrOfJoints());
	
	ROS_INFO("...initialized!");
}

void CobTwistController::run()
{
	ROS_INFO("cob_twist_controller...spinning");
	ros::spin();
}


void CobTwistController::twist_cb(const geometry_msgs::Twist::ConstPtr& msg)
{
	tf::StampedTransform transform_tf;
	KDL::Frame frame;
	try{
		tf_listener_.lookupTransform(chain_base_, chain_tip_, ros::Time(0), transform_tf);
		frame.p = KDL::Vector(transform_tf.getOrigin().x(), transform_tf.getOrigin().y(), transform_tf.getOrigin().z());
		frame.M = KDL::Rotation::Quaternion(transform_tf.getRotation().x(), transform_tf.getRotation().y(), transform_tf.getRotation().z(), transform_tf.getRotation().w());
	}
	catch (tf::TransformException ex){
		ROS_ERROR("%s",ex.what());
		return;
	}
	
	KDL::Twist twist;
	tf::twistMsgToKDL(*msg, twist);
	KDL::JntArray q_dot_ik(chain_.getNrOfJoints());
	
	//ROS_INFO("Twist Vel (%f, %f, %f)", twist.vel.x(), twist.vel.y(), twist.vel.z());
	//ROS_INFO("Twist Rot (%f, %f, %f)", twist.rot.x(), twist.rot.y(), twist.rot.z());
	
	///ToDo: Verify this transformation
	KDL::Twist twist_transformed = frame*twist;
	
	//ROS_INFO("TwistTransformed Vel (%f, %f, %f)", twist_transformed.vel.x(), twist_transformed.vel.y(), twist_transformed.vel.z());
	//ROS_INFO("TwistTransformed Rot (%f, %f, %f)", twist_transformed.rot.x(), twist_transformed.rot.y(), twist_transformed.rot.z());
	
	
	//int ret_ik = p_iksolver_vel_->CartToJnt(last_q_, twist_transformed, q_dot_ik);
	int ret_ik = p_augmented_solver_->CartToJnt(last_q_, twist_transformed, q_dot_ik);
	
	
	if(ret_ik < 0)
	{
		ROS_ERROR("No Vel-IK found!");
	}
	else
	{
		q_dot_ik = normalize_velocities(q_dot_ik);
		
		brics_actuator::JointVelocities vel_msg;
		vel_msg.velocities.resize(joints_.size());
		for(unsigned int i=0; i<dof_; i++)
		{
			vel_msg.velocities[i].joint_uri = joints_[i].c_str();
			vel_msg.velocities[i].unit = "rad";
			vel_msg.velocities[i].value = q_dot_ik(i);
			
			ROS_WARN("DesiredVel %d: %f", i, q_dot_ik(i));
		}
		vel_pub.publish(vel_msg);
	}
}


void CobTwistController::jointstate_cb(const sensor_msgs::JointState::ConstPtr& msg)
{
	KDL::JntArray q_temp = last_q_;
	KDL::JntArray q_dot_temp = last_q_dot_;
	int count = 0;
	
	for(unsigned int j = 0; j < dof_; j++)
	{
		for(unsigned int i = 0; i < msg->name.size(); i++)
		{
			if(strcmp(msg->name[i].c_str(), joints_[j].c_str()) == 0)
			{
				q_temp(j) = msg->position[i];
				q_dot_temp(j) = msg->velocity[i];
				count++;
				break;
			}
		}
	}
	
	if(count == joints_.size())
	{
		//ROS_DEBUG("Done Parsing");
		last_q_ = q_temp;
		last_q_dot_ = q_dot_temp;
	}
}





KDL::JntArray CobTwistController::normalize_velocities(KDL::JntArray q_dot_ik)
{
	KDL::JntArray q_dot_norm = q_dot_ik;
	double max_factor = 1;
	for(unsigned int i=0; i<dof_; i++)
	{
		if(max_factor < (q_dot_ik(i)/limits_vel_[i]))
		{
			max_factor = (q_dot_ik(i)/limits_vel_[i]);
			ROS_WARN("Joint %d exceeds limit: Desired %f, Limit %f, Factor %f", i, q_dot_ik(i), limits_vel_[i], max_factor);
		}
	}
	if(max_factor > 1)
	{
		ROS_INFO("Normalizing velocities!");
		for(unsigned int i=0; i<dof_; i++)
		{
			q_dot_norm(i) = q_dot_ik(i)/max_factor;
			ROS_WARN("Joint %d Normalized %f", i, q_dot_norm(i));
		}
	}
	
	return q_dot_norm;
}




