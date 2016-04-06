// NOTICE:
// GAZEBO uses ISO units, i.e. length uses meters.
// but other code uses cm as the length unit, so for publishing
// and subscribing messages, length unit should be changed to 'cm'

#include <algorithm>
#include <assert.h>
#include <cmath>
#include "rival_gazebo.hh"
#include "vector_angle.hh"

#define RUN 1
#define FLY -1
#define ZERO_VECTOR math::Vector3::Zero
#define PI 3.14159265

#define CM2M_CONVERSION 0.01
#define M2CM_CONVERSION 100

const math::Vector3 kick_vector_nubot(-1,0,0);    // Normalized vector from origin to kicking mechanism in nubot refercence frame.
                                                 // It is subject to nubot model file
const double goal_x = 9.0;
const double goal_height = 1.0;
const double        g = 9.8;
const double        m = 0.41;                   // ball mass (kg)
const double eps = 0.0001;                      // small value

using namespace gazebo;
GZ_REGISTER_MODEL_PLUGIN(RivalGazebo)

RivalGazebo::RivalGazebo()
{
    // Variables initialization
    desired_rot_vector_ = ZERO_VECTOR;
    desired_trans_vector_ = ZERO_VECTOR;
    nubot_football_vector_ = math::Vector3(1,0,0);
    kick_vector_world_ = kick_vector_nubot;
    nubot_football_vector_length_ = 1;
    football_index_=nubot_index_=0;
    Vx_cmd_=Vy_cmd_=w_cmd_=0;
    force_ = 0.0; mode_=1;

    model_count_ = 0;
    dribble_flag_ = false;
    shot_flag_ = false;
    ModelStatesCB_flag_ = false;
    judge_nubot_stuck_ = false;
    is_kick_ = false;
    is_hold_ball_ = false;
    ball_decay_flag_=false;
    AgentID_ = 0;
    state_ = CHASE_BALL;
    sub_state_ = MOVE_BALL;

    obstacles_ = new Obstacles();
    if(!obstacles_)
        ROS_FATAL("Cannot allocate memory to type Obstacles!");
}

RivalGazebo::~RivalGazebo()
{
  event::Events::DisconnectWorldUpdateBegin(update_connection_);
  // Removes all callbacks from the queue. Does not wait for calls currently in progress to finish. 
  message_queue_.clear();
  service_queue_.clear();
  // Disable the queue, meaning any calls to addCallback() will have no effect. 
  message_queue_.disable();
  service_queue_.disable();
  message_callback_queue_thread_.join();
  service_callback_queue_thread_.join();
  rosnode_->shutdown();
  delete rosnode_;
  delete obstacles_;
}

void RivalGazebo::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{   
  // Get the world name.
  world_ = _model->GetWorld();
  nubot_model_ = _model;
  model_name_ = nubot_model_->GetName();
  robot_namespace_ = nubot_model_->GetName();

  // Make sure the ROS node for Gazebo has already been initialized
  if (!ros::isInitialized())
  {
    ROS_FATAL_STREAM("A ROS node for Gazebo has not been initialized, unable to load plugin. "
      << "Load the Gazebo system plugin 'libnubot_gazebo.so' in the gazebo_ros package)");
    return;
  }
  rosnode_ = new ros::NodeHandle(robot_namespace_);
  
  // load parameters
  rosnode_->param("/football/name",                football_name_,          std::string("football") );
  rosnode_->param("/football/chassis_link",        football_chassis_,       std::string("football::ball") );
  rosnode_->param("/cyan/prefix",                 nubot_prefix_,           std::string("nubot"));
  rosnode_->param("/magenta/prefix",                 rival_prefix_,           std::string("rival"));
  rosnode_->param("/general/dribble_distance_thres", dribble_distance_thres_, 0.50);
  rosnode_->param("/general/dribble_angle_thres",    dribble_angle_thres_,    30.0);
  rosnode_->param("/field/length",                 field_length_,           18.0);
  rosnode_->param("/field/width",                  field_width_,            12.0);

  std::string sub_str = model_name_.substr(nubot_prefix_.size(),model_name_.size()-nubot_prefix_.size());    // get the robot id
  const char *environment=sub_str.c_str();
  AgentID_ = atoi(environment);
  ROS_FATAL(" %s has %d plugins, my id is :%d",model_name_.c_str(), nubot_model_->GetPluginCount(), AgentID_);

  // Load the football model 
  football_model_ = world_->GetModel(football_name_);
  if (!football_model_)
  {
    ROS_ERROR("model [%s] does not exist", football_name_.c_str());
  }
  else 
  {
    football_link_ = football_model_->GetLink(football_chassis_);
    if(!football_link_)
    {
      ROS_ERROR("link [%s] does not exist!", football_chassis_.c_str());
    }
  }    
  
  // Publishers
  omin_vision_pub_   = rosnode_->advertise<nubot_common::OminiVisionInfo>("omnivision/OmniVisionInfo",10);

  debug_pub_ = rosnode_->advertise<std_msgs::Float64MultiArray>("debug",10);

  // Subscribers.
  ros::SubscribeOptions so1 = ros::SubscribeOptions::create<gazebo_msgs::ModelStates>(
    "/gazebo/model_states", 100, boost::bind( &RivalGazebo::model_states_CB,this,_1),
    ros::VoidPtr(), &message_queue_);
  ModelStates_sub_ = rosnode_->subscribe(so1);

  ros::SubscribeOptions so2 = ros::SubscribeOptions::create<nubot_common::VelCmd>(
    "nubotcontrol/velcmd", 100, boost::bind( &RivalGazebo::vel_cmd_CB,this,_1),
    ros::VoidPtr(), &message_queue_);
  Velcmd_sub_ = rosnode_->subscribe(so2);

  // Service Servers
  ros::AdvertiseServiceOptions aso1 = ros::AdvertiseServiceOptions::create<nubot_common::BallHandle>(
              "BallHandle", boost::bind(&RivalGazebo::ball_handle_control_service, this, _1, _2),
              ros::VoidPtr(), &service_queue_);
  ballhandle_server_ =   rosnode_->advertiseService(aso1);

  ros::AdvertiseServiceOptions aso2 = ros::AdvertiseServiceOptions::create<nubot_common::Shoot>(
              "Shoot", boost::bind(&RivalGazebo::shoot_control_servive, this, _1, _2),
              ros::VoidPtr(), &service_queue_);
  shoot_server_ =   rosnode_->advertiseService(aso2);

  // Custom Callback Queue Thread. Use threads to process message and service callback queue
  message_callback_queue_thread_ = boost::thread( boost::bind( &RivalGazebo::message_queue_thread,this ) );
  service_callback_queue_thread_ = boost::thread( boost::bind( &RivalGazebo::service_queue_thread,this ) );

  // This event is broadcast every simulation iteration.
  update_connection_ = event::Events::ConnectWorldUpdateBegin(
      boost::bind(&RivalGazebo::update_child, this));
}

void RivalGazebo::Reset()
{
    ROS_DEBUG("%s Reset() running now!", model_name_.c_str());

    // Variables initialization
    desired_rot_vector_ = ZERO_VECTOR;
    desired_trans_vector_ = ZERO_VECTOR;
    nubot_football_vector_ = math::Vector3(1,0,0);
    kick_vector_world_ = kick_vector_nubot;
    nubot_football_vector_length_ = 1;
    football_index_=nubot_index_=0;
    Vx_cmd_=Vy_cmd_=w_cmd_=0;
    force_ = 0.0; mode_=1;

    model_count_ = 0;
    dribble_flag_ = false;
    shot_flag_ = false;
    ModelStatesCB_flag_ = false;
    judge_nubot_stuck_ = false;
    is_kick_ = false;
    is_hold_ball_ = false;
    ball_decay_flag_=false;
    AgentID_ = 0;
    state_ = CHASE_BALL;
    sub_state_ = MOVE_BALL;

}

void RivalGazebo::message_queue_thread()
{
  static const double timeout = 0.01;
  while (rosnode_->ok())
  {
    // Invoke all callbacks currently in the queue. If a callback was not ready to be called,
    // pushes it back onto the queue. This version includes a timeout which lets you specify
    // the amount of time to wait for a callback to be available before returning.
    message_queue_.callAvailable(ros::WallDuration(timeout));
  }
}

void RivalGazebo::service_queue_thread()
{
    static const double timeout = 0.01;
    while (rosnode_->ok())
      service_queue_.callAvailable(ros::WallDuration(timeout));
}

void RivalGazebo::model_states_CB(const gazebo_msgs::ModelStates::ConstPtr& _msg)
{
  msgCB_lock_.lock();

  ModelStatesCB_flag_ = true;
  model_count_ = world_->GetModelCount();
  // Resize message fields
  model_states_msg_.name.resize(model_count_);
  model_states_msg_.pose.resize(model_count_);
  model_states_msg_.twist.resize(model_count_);

  for(int i=0; i<model_count_;i++)
  {
    model_states_msg_.name[i] = _msg->name[i];
    model_states_msg_.pose[i] = _msg->pose[i];   // Reference fram: world
    model_states_msg_.twist[i] = _msg->twist[i];
    if(model_states_msg_.name[i] == football_name_ )
        football_index_ =  i;             
    else if(model_states_msg_.name[i] == model_name_)
        nubot_index_ = i;                     
  }

   msgCB_lock_.unlock();
}

bool RivalGazebo::update_model_info(void)
{  
    rosnode_->param("/general/dribble_distance_thres",    dribble_distance_thres_,    0.50);
    rosnode_->param("/general/dribble_angle_thres",      dribble_angle_thres_,      30.0);

    // Depends on nubot hardware configuration
   if(ModelStatesCB_flag_)
   {
        receive_sim_time_ = world_->GetSimTime();
#if 1 // no Gaussian noise
        // Get football and nubot's pose and twist
        football_state_.model_name = football_name_ ;
        football_state_.pose.position.x     = -model_states_msg_.pose[football_index_].position.x;
        football_state_.pose.position.y     = -model_states_msg_.pose[football_index_].position.y;
        football_state_.pose.position.z     = model_states_msg_.pose[football_index_].position.z;
        football_state_.pose.orientation.w  = model_states_msg_.pose[football_index_].orientation.w;
        football_state_.pose.orientation.x  = model_states_msg_.pose[football_index_].orientation.x;
        football_state_.pose.orientation.y  = model_states_msg_.pose[football_index_].orientation.y;
        football_state_.pose.orientation.z  = model_states_msg_.pose[football_index_].orientation.z;
        football_state_.twist.linear.x      = -model_states_msg_.twist[football_index_].linear.x;
        football_state_.twist.linear.y      = -model_states_msg_.twist[football_index_].linear.y;
        football_state_.twist.linear.z      = model_states_msg_.twist[football_index_].linear.z;
        football_state_.twist.angular.x     = model_states_msg_.twist[football_index_].angular.x;
        football_state_.twist.angular.y     = model_states_msg_.twist[football_index_].angular.y;
        football_state_.twist.angular.z     = model_states_msg_.twist[football_index_].angular.z;

        nubot_state_.model_name = football_name_ ;
        nubot_state_.pose.position.x    = -model_states_msg_.pose[nubot_index_].position.x;
        nubot_state_.pose.position.y    = -model_states_msg_.pose[nubot_index_].position.y;
        nubot_state_.pose.position.z    = model_states_msg_.pose[nubot_index_].position.z;
        // Rot(z, 180 degrees); [d a b c] ==> [-c b -a d]
        double d = model_states_msg_.pose[nubot_index_].orientation.w;
        double a = model_states_msg_.pose[nubot_index_].orientation.x;
        double b = model_states_msg_.pose[nubot_index_].orientation.y;
        double c = model_states_msg_.pose[nubot_index_].orientation.z;
        nubot_state_.pose.orientation.w = -c;
        nubot_state_.pose.orientation.x = b;
        nubot_state_.pose.orientation.y = -a;
        nubot_state_.pose.orientation.z = d;

        nubot_state_.twist.linear.x     = -model_states_msg_.twist[nubot_index_].linear.x;
        nubot_state_.twist.linear.y     = -model_states_msg_.twist[nubot_index_].linear.y;
        nubot_state_.twist.linear.z     = model_states_msg_.twist[nubot_index_].linear.z;
        nubot_state_.twist.angular.x    = model_states_msg_.twist[nubot_index_].angular.x;
        nubot_state_.twist.angular.y    = model_states_msg_.twist[nubot_index_].angular.y;
        nubot_state_.twist.angular.z    = model_states_msg_.twist[nubot_index_].angular.z;
#endif
#if 0 // add gaussian noise
        static double scalar = 0.0167;
        football_state_.model_name = football_name_ ;
        football_state_.pose.position.x     =  model_states_msg_.pose[football_index_].position.x + scalar*rand_.GetDblNormal(0,1);
        football_state_.pose.position.y     =  model_states_msg_.pose[football_index_].position.y + scalar*rand_.GetDblNormal(0,1);
        football_state_.pose.position.z     =  model_states_msg_.pose[football_index_].position.z;
//        football_state_.pose.orientation.w  =  model_states_msg_.pose[football_index_].orientation.w;
//        football_state_.pose.orientation.x  =  model_states_msg_.pose[football_index_].orientation.x;
//        football_state_.pose.orientation.y  =  model_states_msg_.pose[football_index_].orientation.y;
//        football_state_.pose.orientation.z  =  model_states_msg_.pose[football_index_].orientation.z;
        football_state_.twist.linear.x      =  model_states_msg_.twist[football_index_].linear.x + scalar*rand_.GetDblNormal(0,1);
        football_state_.twist.linear.y      =  model_states_msg_.twist[football_index_].linear.y + scalar*rand_.GetDblNormal(0,1);
        football_state_.twist.linear.z      =  model_states_msg_.twist[football_index_].linear.z;
//        football_state_.twist.angular.x     =  model_states_msg_.twist[football_index_].angular.x;
//        football_state_.twist.angular.y     =  model_states_msg_.twist[football_index_].angular.y;
//        football_state_.twist.angular.z     =  model_states_msg_.twist[football_index_].angular.z;

        nubot_state_.model_name = model_name_ ;
        nubot_state_.pose.position.x    =  model_states_msg_.pose[nubot_index_].position.x + scalar*rand_.GetDblNormal(0,1);
        nubot_state_.pose.position.y    =  model_states_msg_.pose[nubot_index_].position.y + scalar*rand_.GetDblNormal(0,1);
        nubot_state_.pose.position.z    =  model_states_msg_.pose[nubot_index_].position.z;
        nubot_state_.pose.orientation.w =  model_states_msg_.pose[nubot_index_].orientation.w;
        nubot_state_.pose.orientation.x =  model_states_msg_.pose[nubot_index_].orientation.x;
        nubot_state_.pose.orientation.y =  model_states_msg_.pose[nubot_index_].orientation.y;
        nubot_state_.pose.orientation.z =  model_states_msg_.pose[nubot_index_].orientation.z;
        nubot_state_.twist.linear.x     =  model_states_msg_.twist[nubot_index_].linear.x + scalar*rand_.GetDblNormal(0,1);
        nubot_state_.twist.linear.y     =  model_states_msg_.twist[nubot_index_].linear.y + scalar*rand_.GetDblNormal(0,1);
        nubot_state_.twist.linear.z     =  model_states_msg_.twist[nubot_index_].linear.z;
        nubot_state_.twist.angular.x    =  model_states_msg_.twist[nubot_index_].angular.x;
        nubot_state_.twist.angular.y    =  model_states_msg_.twist[nubot_index_].angular.y;
        nubot_state_.twist.angular.z    =  model_states_msg_.twist[nubot_index_].angular.z + scalar*rand_.GetDblNormal(0,1);

        debug_msgs_.data.clear();
        debug_msgs_.data.push_back(scalar*rand_.GetDblNormal(0,1));
        debug_pub_.publish(debug_msgs_);
#endif

        // calculate vector from nubot to football
        nubot_football_vector_ = football_state_.pose.position - nubot_state_.pose.position;
        nubot_football_vector_length_ = nubot_football_vector_.GetLength();
        //ROS_INFO("nubot_football_vector_length_:%f", nubot_football_vector_length_);

        // transform kick_vector_nubot in world frame
        math::Quaternion    rotation_quaternion = nubot_state_.pose.orientation;
        math::Matrix3       RotationMatrix3 = rotation_quaternion.GetAsMatrix3();
        kick_vector_world_ = RotationMatrix3 * kick_vector_nubot; // vector from nubot origin to kicking mechanism in world frame
        // ROS_INFO("kick_vector_world_: %f %f %f",kick_vector_world_.x, kick_vector_world_.y, kick_vector_world_.z);

        std::string             sub_str;
        std::string             robot_name;
        geometry_msgs::Pose     robot_pose;
        geometry_msgs::Twist    robot_twist;
        int robot_id;
        obstacles_->world_obstacles_.reserve(20);
        obstacles_->real_obstacles_.reserve(20);
        obstacles_->world_obstacles_.clear();
        obstacles_->real_obstacles_.clear();
        omin_vision_info_.robotinfo.reserve(10);
        omin_vision_info_.robotinfo.clear();
        for(int i=0; i<model_count_;i++)
        {
            // Obstacles info
            if(model_states_msg_.name[i].compare(0, nubot_prefix_.size(), nubot_prefix_) == 0 ||
                    model_states_msg_.name[i].compare(0, rival_prefix_.size(), rival_prefix_) == 0)   //compare model name' prefix to determine robots
            {
                math::Vector3 obstacle_position(-model_states_msg_.pose[i].position.x,
                                                -model_states_msg_.pose[i].position.y,
                                                model_states_msg_.pose[i].position.z);
                if(i != nubot_index_)
                {
                    obstacles_->world_obstacles_.push_back(nubot::DPoint(obstacle_position.x,
                                                                         obstacle_position.y));

                    math::Vector3 nubot_obstacle_vector = obstacle_position - nubot_state_.pose.position;   // vector from nubot to obstacle
                    obstacles_->real_obstacles_.push_back( nubot::PPoint( get_angle_PI(kick_vector_world_,nubot_obstacle_vector),
                                                                          nubot_obstacle_vector.GetLength()) );
                }
            }

            // Nubot info
            if(model_states_msg_.name[i].compare(0, rival_prefix_.size(), rival_prefix_) == 0)
            {
                robot_name = model_states_msg_.name[i];
                sub_str = robot_name.substr(rival_prefix_.size(),robot_name.size()-rival_prefix_.size());    // get the robot id
                robot_id = atoi( sub_str.c_str() );

                // get my own and teammates's info
                robot_pose = model_states_msg_.pose[i];
                robot_twist = model_states_msg_.twist[i];
                math::Quaternion rot_qua(robot_pose.orientation.w, robot_pose.orientation.x,
                                          robot_pose.orientation.y, robot_pose.orientation.z);
                double heading_theta = rot_qua.GetYaw();
                robot_info_.header.seq++;
                robot_info_.AgentID       = robot_id;
                robot_info_.pos.x         = -robot_pose.position.x * M2CM_CONVERSION;
                robot_info_.pos.y         = -robot_pose.position.y * M2CM_CONVERSION;
                robot_info_.heading.theta = heading_theta;
                robot_info_.vrot          = robot_twist.angular.z;
                robot_info_.vtrans.x      = -robot_twist.linear.x * M2CM_CONVERSION;
                robot_info_.isvalid       = true;
                robot_info_.vtrans.y      = -robot_twist.linear.y * M2CM_CONVERSION;
                robot_info_.isstuck       = get_nubot_stuck();
                omin_vision_info_.robotinfo.push_back(robot_info_);

            }
        }

        return 1;
   }
   else
   {
        ROS_INFO("%s update_model_info(): Waiting for model_states messages!", model_name_.c_str());
       return 0;
   }
}

void RivalGazebo::message_publish(void)
{
    ros::Time simulation_time(receive_sim_time_.sec, receive_sim_time_.nsec);
    math::Quaternion    rotation_quaternion=nubot_state_.pose.orientation;

    ////////////// OminiVision message /////////////////////////

    ball_info_.header.stamp = simulation_time;
    ball_info_.header.seq++;
    //ball_info_.ballinfostate =
    ball_info_.pos.x =  football_state_.pose.position.x * M2CM_CONVERSION;
    ball_info_.pos.y =  football_state_.pose.position.y * M2CM_CONVERSION;
    ball_info_.real_pos.angle  = get_angle_PI(kick_vector_world_,nubot_football_vector_);
    ball_info_.real_pos.radius = nubot_football_vector_length_ * M2CM_CONVERSION;
    ball_info_.velocity.x = football_state_.twist.linear.x * M2CM_CONVERSION;
    ball_info_.velocity.y = football_state_.twist.linear.y * M2CM_CONVERSION;
    ball_info_.pos_known = true;
    ball_info_.velocity_known = true;

    obstacles_info_.header.stamp = ros::Time::now();
    obstacles_info_.header.seq++;
    obstacles_info_.pos.clear();
    obstacles_info_.polar_pos.clear();
    int length= obstacles_->real_obstacles_.size();

    nubot_common::Point2d point;                                    // message type in ObstaclesInfo.msg
    nubot_common::PPoint  polar_point;                              // message type in ObstaclesInfo.msg
    for(int i = 0 ; i < length ; i++)
    {
        nubot::DPoint & pt=obstacles_->world_obstacles_[i];         // DPoint is a type in nubot/core/core.hpp
        nubot::PPoint & polar_pt= obstacles_->real_obstacles_[i];   // PPoint is a type in nubot/core/core.hpp

        point.x=pt.x_ * M2CM_CONVERSION ;                           // Get the location info and put it in message type
        point.y=pt.y_ * M2CM_CONVERSION;
        polar_point.angle=polar_pt.angle_.radian_;
        polar_point.radius=polar_pt.radius_;

        obstacles_info_.pos.push_back(point);
        obstacles_info_.polar_pos.push_back(polar_point);
    }
    omin_vision_info_.header.stamp = robot_info_.header.stamp;
    omin_vision_info_.header.seq++;
    omin_vision_info_.ballinfo=ball_info_;
    omin_vision_info_.obstacleinfo=obstacles_info_;
    omin_vision_pub_.publish(omin_vision_info_);

}

void RivalGazebo::nubot_locomotion(math::Vector3 linear_vel_vector, math::Vector3 angular_vel_vector)
{
    desired_trans_vector_ = linear_vel_vector;
    desired_rot_vector_   = angular_vel_vector;
    // planar movement
    desired_trans_vector_.z = 0;
    desired_rot_vector_.x = 0;
    desired_rot_vector_.y = 0;
    nubot_model_->SetLinearVel(desired_trans_vector_);
    nubot_model_->SetAngularVel(desired_rot_vector_);
    judge_nubot_stuck_ = 1;                                                 // only afetr nubot tends to move can I judge if it is stuck
}

void RivalGazebo::vel_cmd_CB(const nubot_common::VelCmd::ConstPtr& cmd)
{
   msgCB_lock_.lock();

    Vx_cmd_ = -cmd->Vx * CM2M_CONVERSION;
    Vy_cmd_ = -cmd->Vy * CM2M_CONVERSION;
    w_cmd_  = cmd->w;
    math::Vector3 Vx_nubot = Vx_cmd_ * kick_vector_world_;
    math::Vector3 Vy_nubot = Vy_cmd_ * (math::Vector3(0,0,1).Cross(kick_vector_world_));    // velocity with reference to nubot
    math::Vector3 linear_vector = Vx_nubot + Vy_nubot;
    math::Vector3 angular_vector(0,0,w_cmd_);

//    ROS_FATAL("%s vel_cmd_CB():linear_vector:%f %f %f angular_vector:0 0 %f",model_name_.c_str(),
//                    linear_vector.x, linear_vector.y, linear_vector.z, angular_vector.z);
    nubot_locomotion(linear_vector, angular_vector);

    msgCB_lock_.unlock();
}

bool RivalGazebo::ball_handle_control_service(nubot_common::BallHandle::Request  &req,
                                            nubot_common::BallHandle::Response &res)
{
    srvCB_lock_.lock();

    dribble_flag_ = req.enable ? 1 : 0;         // FIXME. when robot is stucked, req.enable=2
    if(dribble_flag_)
    {
        if(!get_is_hold_ball())     // when dribble_flag is true, it does not necessarily mean that I can dribble it now.
        {                           // it just means the dribble ball mechanism is working.
            dribble_flag_ = false;
            res.BallIsHolding = false;
            ROS_INFO("%s dribble_service: Cannot dribble ball. angle error:%f distance error: %f",
                                          model_name_.c_str(), angle_error_degree_, nubot_football_vector_length_);
        }
        else
        {
            //ROS_INFO("%s dribble_service: dribbling ball now", model_name_.c_str());
            res.BallIsHolding = true;
        }
    }
    else
    {
        res.BallIsHolding = get_is_hold_ball();
    }

    ROS_FATAL("%s dribble_service: req.enable:%d res.ballisholding:%d",model_name_.c_str(), (int)req.enable, (int)res.BallIsHolding);
    srvCB_lock_.unlock();
    return true;
}

bool RivalGazebo::shoot_control_servive( nubot_common::Shoot::Request  &req,
                                       nubot_common::Shoot::Response &res )
{
    srvCB_lock_.lock();

    force_ = (double)req.strength;
    mode_ = (int)req.ShootPos;
    if(force_ > 15.0)
    {
        ROS_FATAL("Kick ball force(%f) is too great.", force_);
        force_ = 15.0;
    }
    if( force_ )
    {
        if(get_is_hold_ball())
        {
            dribble_flag_ = false;
            shot_flag_ = true;
            //ROS_INFO("%s shoot_service: ShootPos:%d strength:%f",model_name_.c_str(), mode_, force_);
            res.ShootIsDone = 1;
        }
        else
        {
            shot_flag_ = false;
            res.ShootIsDone = 0;
            //ROS_INFO("%s shoot_service(): Cannot kick ball. angle error:%f distance error: %f. ",
            //                            model_name_.c_str(), angle_error_degree_, nubot_football_vector_length_);
        }
    }
    else
    {
        shot_flag_ = false;
        res.ShootIsDone = 1;
        //ROS_ERROR("%s shoot_control_service(): Kick-mechanism charging complete!",model_name_.c_str());
    }

    //ROS_INFO("%s shoot_service: req.strength:%f req.shootpos:%d res.shootisdone:%d",
    //            model_name_.c_str(), force_, mode_, (int)res.ShootIsDone);

    srvCB_lock_.unlock();
    return true;
}

void RivalGazebo::dribble_ball(void)
{

#if 1
    math::Quaternion    target_rot = nubot_state_.pose.orientation; //nubot_model_->GetWorldPose().rot;
    math::Vector3       relative_pos = kick_vector_world_* 0.43;
    math::Vector3       target_pos = -(nubot_state_.pose.position + relative_pos);//nubot_model_->GetWorldPose().pos + relative_pos;
    ROS_INFO("%s nubot_pose %f %f kick_vector_world:%f %f",model_name_.c_str(), nubot_state_.pose.position.x, nubot_state_.pose.position.y,
                                                         kick_vector_world_.x, kick_vector_world_.y );
    math::Pose          target_pose(target_pos, target_rot);
    football_model_->SetWorldPose(target_pose);
    football_state_.twist.linear = nubot_state_.twist.linear;
#endif
#if 0
    const static double desired_nubot_football_vector_length =  dribble_distance_thres_;
    if(football_state_.pose.position.z > 0.3)   // if football is in the air, cannot dribble
    {
        ROS_ERROR("dribble_ball(): ball is in the air at %f; return!", football_state_.pose.position.z);
        return;
    }

    math::Vector3     nubot_linear_vel = nubot_model_->GetWorldLinearVel();
    math::Vector3     nubot_angular_vel = nubot_model_->GetWorldAngularVel();
    nubot_linear_vel.z=0; nubot_angular_vel.x=0; nubot_angular_vel.y=0;
    // Set up the direction from nubot to football. Let vector lies in x-y plane
    nubot_football_vector_.z = 0;                             // don't point to the air
    math::Vector3     perpencular_vel = nubot_angular_vel.Cross(nubot_football_vector_);
    math::Vector3     football_vel = nubot_linear_vel + perpencular_vel;
    set_ball_vel(football_vel, ball_decay_flag_);

    ROS_INFO("%s dribble_ball(): dribbling ball. ball vel:%f %f", model_name_.c_str(),football_vel.x, football_vel.y);
#endif
}

void RivalGazebo::kick_ball(int mode, double vel=20.0)
{
   math::Vector3 kick_vector_planar(kick_vector_world_.x, kick_vector_world_.y, 0.0);


   if(mode == RUN)
   {
        double vel2 = vel * 2.3;;                         //FIXME. CAN TUNE
        math::Vector3 vel_vector = -kick_vector_planar * vel2;
        set_ball_vel(vel_vector, ball_decay_flag_);
        ROS_INFO("kick ball vel:%f vel2:%f",vel, vel2);
   }
   else if(mode == FLY)
   {
        // math formular: y = a*x^2 + b*x + c;
        //  a = -g/(2*vx*vx), c = 0, b = kick_goal_height/D + g*D/(2.0*vx*vx)
        //  mid_point coordinates:[-b/(2*a), (4a*c-b^2)/(4a) ]

        static const double g = 9.8, kick_goal_height = goal_height - 0.20;      // FIXME: can be tuned
        nubot::DPoint point1(nubot_state_.pose.position.x,nubot_state_.pose.position.y);
        nubot::DPoint point2(nubot_state_.pose.position.x + kick_vector_world_.x,
                             nubot_state_.pose.position.y + kick_vector_world_.y);
        nubot::DPoint point3(football_state_.pose.position.x,football_state_.pose.position.y);
        nubot::Line_ line1(point1, point2);
        nubot::Line_ line2(1.0, 0.0, kick_vector_world_.x>0 ? -goal_x : goal_x);         // nubot::Line_(A,B,C);

        nubot::DPoint crosspoint = line1.crosspoint(line2);
        double D = crosspoint.distance(point3);
        double vx_thres = D*sqrt(g/2/kick_goal_height);
        double vx = vx_thres/2.0;//>vel ? vel : vx_thres/2.0;                            // initial x velocity.CAN BE TUNED
        double b = kick_goal_height/D + g*D/(2.0*vx*vx);

        ROS_INFO("%s crosspoint:(%f %f) vx: %f", model_name_.c_str(),
                 crosspoint.x_, crosspoint.y_, vx);
        if( fabs(crosspoint.y_) < 10)
        {
            math::Vector3 kick_vector(-vx*kick_vector_world_.x, -vx*kick_vector_world_.y, b*vx);
            set_ball_vel(kick_vector, ball_decay_flag_);
        }
        else
            ROS_FATAL("CANNOT SHOOT. crosspoint.y is too big!");
   }
   else
   {
       ROS_ERROR("%s kick_ball(): Incorrect mode!", model_name_.c_str());
   }
}

bool RivalGazebo::get_is_hold_ball(void)
{
    bool near_ball, allign_ball;
    math::Vector3 norm = nubot_football_vector_;
    norm.z=0.0; norm.Normalize();
    kick_vector_world_.z=0.0;
    angle_error_degree_ = get_angle_PI(kick_vector_world_,norm)*(180/PI);
    //ROS_INFO("kick_vector:%f %f, nubot_ball:%f %f", kick_vector_world_.x, kick_vector_world_.y,
    //                                                nubot_football_vector_.x, nubot_football_vector_.y);

    allign_ball = (angle_error_degree_ <= dribble_angle_thres_/2.0
                    && angle_error_degree_ >= -dribble_angle_thres_/2.0) ?
                    1 : 0;
    near_ball = nubot_football_vector_length_ <= dribble_distance_thres_ ?
                1 : 0;

    //ROS_INFO("%s get_is_hold_ball(): angle error:%f(thres:%f) distance_error:%f(thres:%f)",
    //         model_name_.c_str(),  angle_error_degree_, dribble_angle_thres_,
    //         nubot_football_vector_length_, dribble_distance_thres_);

    return (near_ball && allign_ball);
}

bool RivalGazebo::get_nubot_stuck(void)
{
    static int time_count=0;
    static bool last_time_stuck=0;
    static const int time_limit = 40;
    static bool is_stuck;

    if(judge_nubot_stuck_)
    {
        judge_nubot_stuck_ = 0;
        static const double scale = 0.5;                                    // FIXME. Can tune
        double desired_trans_length = desired_trans_vector_.GetLength();
        double desired_rot_length   = desired_rot_vector_.z>0 ?
                                        desired_rot_vector_.z :-desired_rot_vector_.z ;
        double actual_trans_length  = nubot_state_.twist.linear.GetLength();
        double actual_rot_length    = nubot_state_.twist.angular.z>0 ?
                                        nubot_state_.twist.angular.z : -nubot_state_.twist.angular.z;

        //ROS_INFO("%s time_count:%d, last_time_stuck:%d",model_name_.c_str(), time_count, last_time_stuck);
        //ROS_INFO("desired_trans_len:%f actual_trans_len:%f",desired_trans_length,actual_trans_length);
        //ROS_INFO("desired_rot_len:%f actual_rot_len:%f",desired_rot_length, actual_rot_length);

        if(actual_trans_length < desired_trans_length * scale)
        {
            if(last_time_stuck)
                time_count++;
            else
                time_count = 0;

            last_time_stuck = 1;
            if(time_count > time_limit)
            {
                // ROS_INFO("get_nubot_stuck(): desired_trans:%f actual_trans:%f", desired_trans_length, actual_trans_length);
                // ROS_INFO("%s get_nubot_stuck(): cannot translate!", model_name_.c_str());
                time_count = 0;
                is_stuck = 1;
            }
        }
        else if(actual_rot_length < desired_rot_length * scale)
        {
            if(last_time_stuck)
                time_count++;
            else
                time_count = 0;

            last_time_stuck = 1;
            if(time_count > time_limit)
            {
                // ROS_INFO("desired_rot:%f actual_rot:%f", desired_rot_length, actual_rot_length);
                // ROS_ERROR("%s get_nubot_stuck(): cannot rotate!", model_name_.c_str());
                time_count = 0;
                is_stuck = 1;
            }
        }
        else
        {
            last_time_stuck = 0;
            is_stuck = 0;
        }

        return is_stuck;
    }
    else
    {
        //ROS_FATAL("%s judge_nubot_stuck_flag not set!", model_name_.c_str());
        return 0;
    }
}

void RivalGazebo::update_child()
{
    msgCB_lock_.lock(); // lock access to fields that are used in ROS message callbacks
    srvCB_lock_.lock();
    /* delay in model_states messages publishing
     * so after receiving model_states message, then nubot moves. */
    if(update_model_info())
    {
        /********** EDIT BEGINS **********/

        detect_ball_out();
        nubot_be_control();
        // nubot_test();

        /**********  EDIT ENDS  **********/
    }

    if(ball_decay_flag_)
    {
        math::Vector3 free_ball_vel = football_state_.twist.linear;
        ball_vel_decay(free_ball_vel, 0.3);
    }
    ball_decay_flag_ = true;

    srvCB_lock_.unlock();
    msgCB_lock_.unlock();
}

void RivalGazebo::nubot_be_control(void)
{
    static int count=0;
    if(nubot_state_.pose.position.z < 0.2)      // not in the air
    {
        if(dribble_flag_)                       // dribble_flag_ is set by BallHandle service
             dribble_ball();

        if(shot_flag_)
        {
            kick_ball(mode_, force_);
            shot_flag_ = false;
        }
    }
    else
        ROS_FATAL("%s in the air!",model_name_.c_str());

    message_publish();                          // publish message to world_model node
}

void RivalGazebo::detect_ball_out(void)
{
    if(fabs(football_state_.pose.position.x)>field_length_/2.0+1 ||
            fabs(football_state_.pose.position.y)> field_width_/2.0+1)
    {
        football_model_->SetLinearVel(ZERO_VECTOR);
        int a = football_state_.pose.position.x > 0? 1 : -1;
        int b = football_state_.pose.position.y > 0? 1 : -1;
        double new_x = a * (field_length_/2.0 + 0.5);
        double new_y = b * (field_width_/2.0 + 0.5);
        math::Pose  target_pose( math::Vector3 (new_x, new_y, 0), math::Quaternion(0,0,0) );
        football_model_->SetWorldPose(target_pose);
    }
}

void RivalGazebo::ball_vel_decay(math::Vector3 vel, double mu)
{
    static double last_vel_len = vel.GetLength();
    double vel_len = vel.GetLength();

    if(vel_len > 0.0)
    {
        if(football_state_.pose.position.z <= 0.12 &&
                !(last_vel_len - vel_len > 0) )
        {
            static double force = mu*m*g;
            football_link_->AddForce(vel.Normalize()*force);
        }
    }
    else if(vel_len < 0)
    {
        vel_len = 0.0;
        vel = ZERO_VECTOR;
        set_ball_vel(vel, ball_decay_flag_);
    }

    last_vel_len = vel_len;
}

void RivalGazebo::set_ball_vel(math::Vector3 &vel, bool &ball_decay_flag)
{
    football_model_->SetLinearVel(vel);
    ball_decay_flag_ = false;                        // setting linear vel to ball indicates it is not free rolling
                                                    // so no additional friction is applied now
}

void RivalGazebo::nubot_test(void)
{
// dribble ball
#if 0
    nubot_locomotion(math::Vector3(5,0,0),math::Vector3(0,0,2);
    dribble_ball();
    ROS_INFO("nubot-football distance:%f",nubot_football_vector_length_);
#endif
// kick ball
#if 0
    static bool flag=1;
    if(flag)
    {
        kick_ball(FLY, 20);
        flag = 0;
    }
#endif
// get nubot stuck flag test
#if 0
    bool a=get_nubot_stuck();
    ROS_FATAL("%d",a);
    nubot_locomotion(math::Vector3(0,0,0),math::Vector3(0,0,1));
#endif
//for testing velocity decay
#if 0
    static int count=0;
    math::Vector3 vel(3,0,0);
    if(count++<50)
    {
        set_ball_vel(vel, ball_decay_flag_);
        nubot_model_->SetLinearVel(math::Vector3(2,0,0));
    }
    debug_msgs_.data.clear();
    double data0 = football_model_->GetWorldLinearVel().GetLength();
    debug_msgs_.data.push_back(data0);
    debug_pub_.publish(debug_msgs_);
#endif
// for testing time duration
#if 0
    common::Time                last_update_time_;
    last_update_time_ = world_->GetSimTime();
    for(int i=0; i<50; i++)
    {
        kick_ball(goal0_pos, mode, force);
        ROS_INFO("%s is kicking ball!",model_name_.c_str());
    }
    common::Time current_time = world_->GetSimTime();
    double seconds_since_last_update = ( current_time - last_update_time_ ).Double();
    ROS_FATAL("kick time:%f",seconds_since_last_update);
#endif
#if 0
    double vel = nubot_model_->GetWorldLinearVel().GetLength();
    //double vel2 = nubot_state_.twist.linear.x;
    nubot_locomotion(math::Vector3(1,0,0),ZERO_VECTOR);
    //ROS_INFO("function:%f state:%f",vel,vel2);
    debug_msgs_.data.clear();
    debug_msgs_.data.push_back(vel);
    //debug_msgs_.data.push_back(vel2);
    debug_pub_.publish(debug_msgs_);
#endif
}