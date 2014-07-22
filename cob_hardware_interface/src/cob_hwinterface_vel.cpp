#include <cob_hardware_interface/cob_hwinterface_vel.h>

CobHWInterfaceVel::CobHWInterfaceVel()
{ 
    //Get joint names from parameter server
    
    std::string param="joint_names";
    XmlRpc::XmlRpcValue jointNames_XMLRPC;
    if (n.hasParam(param))
    {
        n.getParam(param, jointNames_XMLRPC);
    }
    else
    {
        ROS_ERROR("Parameter %s not set, shutting down node...", param.c_str());
        n.shutdown();
    }

    for (int i=0; i<jointNames_XMLRPC.size(); i++)
        joint_names.push_back(static_cast<std::string>(jointNames_XMLRPC[i]));  
   
    pos.resize(joint_names.size());
    vel.resize(joint_names.size());
    eff.resize(joint_names.size());
    cmd.resize(joint_names.size());
    
    pos_temp.resize(joint_names.size());
    vel_temp.resize(joint_names.size());
    eff_temp.resize(joint_names.size());
    cmd_temp.resize(joint_names.size());
   
    for(unsigned int i=0; i<joint_names.size(); i++)
    {
        // connect and register the joint state interface
        hardware_interface::JointStateHandle state_handle(joint_names[i], &pos[i], &vel[i], &eff[i]);
        jnt_state_interface.registerHandle(state_handle);
        
        // connect and register the joint velocity interface
        hardware_interface::JointHandle vel_handle(jnt_state_interface.getHandle(joint_names[i]), &cmd[i]);
        jnt_vel_interface.registerHandle(vel_handle);
    }

    registerInterface(&jnt_state_interface);
    registerInterface(&jnt_vel_interface);
   
    //initialize the according subscriber and publisher here   
    pub = n.advertise<brics_actuator::JointVelocities>("command_vel",1);
    sub = n.subscribe("state",1, &CobHWInterfaceVel::state_callback,this);
}

void CobHWInterfaceVel::state_callback(const control_msgs::JointTrajectoryControllerState::ConstPtr& msg)
{
    //save the contents of the message in temporary vectors
    
    if(msg->actual.positions.size()!=joint_names.size())
    {
        ROS_ERROR("Message size different from number of joints");
    }
    else
    {
        for (unsigned int i=0; i<joint_names.size(); i++) 
        {
            pos_temp[i]=msg->actual.positions[i];        
            vel_temp[i]=msg->actual.velocities[i];      
            eff_temp[i]=msg->actual.effort[i];        
        }        
    }
}

void CobHWInterfaceVel::read()
{
    //pos, vel, eff are updated!
    //this should be done by safely (access-control!) writing the cu    rrent values that were received through a callback function for the subscription to /joint_states or the respective hw-driver directly 
    
    for (unsigned int i=0; i<joint_names.size(); i++) 
    {
        pos[i]=pos_temp[i];
        vel[i]=vel_temp[i];
        eff[i]=eff_temp[i];
    }
    return;  
}


void CobHWInterfaceVel::write()
{
    //here the current cmd values need to be propagated to the actual hardware
    //cmd contains the desired velocity in this case
    //this should be done by publishing them to the accortind command_vel (or command_direct) topic of the hw-driver
    
    brics_actuator::JointVelocities command;    
    
    for (unsigned int i=0; i<joint_names.size(); i++) 
    {        
        brics_actuator::JointValue value;
        value.value=cmd[i];
        command.velocities.push_back(value);
    }
       
    pub.publish(command);
    
    return;
}
