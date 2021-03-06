/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014, CU Boulder
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of CU Boulder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/**
 * \brief   Simple pick place for blocks
 * \author  Dave Coleman
 */

// ROS
#include <ros/ros.h>

#include <std_msgs/Empty.h>

// MoveIt!
#include <moveit/move_group_interface/move_group.h>

// Grasp generation
#include <moveit_simple_grasps/simple_grasps.h>
#include <moveit_simple_grasps/grasp_data.h>
#include <moveit_visual_tools/visual_tools.h> // simple tool for showing graspsp

static const double BLOCK_SIZE = 0.04;

struct MetaBlock
{
    std::string name;
    geometry_msgs::Pose start_pose;
    geometry_msgs::Pose goal_pose;
};

// grasp generator
moveit_simple_grasps::SimpleGraspsPtr simple_grasps_;

moveit_visual_tools::VisualToolsPtr visual_tools_;

// data for generating grasps
moveit_simple_grasps::GraspData grasp_data_;

// our interface with MoveIt
boost::scoped_ptr<move_group_interface::MoveGroup> move_group_;

// which arm are we using
std::string ee_group_name_;
std::string planning_group_name_;

ros::Publisher pub_arm_done;
ros::Subscriber sub_turtle_done_;

// settings
bool auto_reset_(false);
int auto_reset_sec_(4);
int pick_place_count_(0); // tracks how many pick_places have run

void resetBlock(MetaBlock block)
{
    // Remove attached object
    visual_tools_->cleanupACO(block.name);

    // Remove collision object
    visual_tools_->cleanupCO(block.name);

    // Add the collision block
    visual_tools_->publishCollisionBlock(block.start_pose, block.name, BLOCK_SIZE);
}

MetaBlock createStartBlock(double x, double y, const std::string name)
{
    MetaBlock start_block;
    start_block.name = name;

    // Position
    start_block.start_pose.position.x = x;
    start_block.start_pose.position.y = y;
    start_block.start_pose.position.z = BLOCK_SIZE / 2.0; //getTableHeight(-0.9);

    // Orientation
    double angle = 0; // M_PI / 1.5;
    Eigen::Quaterniond quat(Eigen::AngleAxis<double>(double(angle), Eigen::Vector3d::UnitZ()));
    start_block.start_pose.orientation.x = quat.x();
    start_block.start_pose.orientation.y = quat.y();
    start_block.start_pose.orientation.z = quat.z();
    start_block.start_pose.orientation.w = quat.w();

    return start_block;
}

bool pick(const geometry_msgs::Pose& block_pose, std::string block_name)
{
    std::vector<moveit_msgs::Grasp> possible_grasps;

    // Pick grasp
    simple_grasps_->generateBlockGrasps( block_pose, grasp_data_, possible_grasps );

    // Visualize them
    //visual_tools_->publishAnimatedGrasps(possible_grasps, grasp_data_.ee_parent_link_);
    visual_tools_->publishGrasps(possible_grasps, grasp_data_.ee_parent_link_);

    // Prevent collision with table
    //move_group_->setSupportSurfaceName(SUPPORT_SURFACE3_NAME);

    // Allow blocks to be touched by end effector
    {
        // an optional list of obstacles that we have semantic information about and that can be touched/pushed/moved in the course of grasping
        std::vector<std::string> allowed_touch_objects;
        allowed_touch_objects.push_back("Block1");
        allowed_touch_objects.push_back("Block2");
        allowed_touch_objects.push_back("Block3");
        allowed_touch_objects.push_back("Block4");

        // Add this list to all grasps
        for (std::size_t i = 0; i < possible_grasps.size(); ++i)
        {
            possible_grasps[i].allowed_touch_objects = allowed_touch_objects;
        }
    }

    //ROS_INFO_STREAM_NAMED("","Grasp 0\n" << possible_grasps[0]);

    return move_group_->pick(block_name, possible_grasps);
}

bool place(const geometry_msgs::Pose& goal_block_pose, std::string block_name)
{
    ROS_WARN_STREAM_NAMED("place","Placing '"<< block_name << "'");

    std::vector<moveit_msgs::PlaceLocation> place_locations;

    // Re-usable datastruct
    geometry_msgs::PoseStamped pose_stamped;
    pose_stamped.header.frame_id = grasp_data_.base_link_;
    pose_stamped.header.stamp = ros::Time::now();

    // Create 360 degrees of place location rotated around a center
    for (double angle = 0; angle < 2*M_PI; angle += M_PI/2)
    {
        pose_stamped.pose = goal_block_pose;

        // Orientation
        Eigen::Quaterniond quat(Eigen::AngleAxis<double>(double(angle), Eigen::Vector3d::UnitZ()));
        pose_stamped.pose.orientation.x = quat.x();
        pose_stamped.pose.orientation.y = quat.y();
        pose_stamped.pose.orientation.z = quat.z();
        pose_stamped.pose.orientation.w = quat.w();

        // Create new place location
        moveit_msgs::PlaceLocation place_loc;

        place_loc.place_pose = pose_stamped;

        visual_tools_->publishBlock( place_loc.place_pose.pose, moveit_visual_tools::BLUE, BLOCK_SIZE);

        // Approach
        moveit_msgs::GripperTranslation pre_place_approach;
        pre_place_approach.direction.header.stamp = ros::Time::now();
        pre_place_approach.desired_distance = grasp_data_.approach_retreat_desired_dist_; // The distance the origin of a robot link needs to travel
        pre_place_approach.min_distance = grasp_data_.approach_retreat_min_dist_; // half of the desired? Untested.
        pre_place_approach.direction.header.frame_id = grasp_data_.base_link_;
        pre_place_approach.direction.vector.x = 0;
        pre_place_approach.direction.vector.y = 0;
        pre_place_approach.direction.vector.z = -1; // Approach direction (negative z axis)  // TODO: document this assumption
        place_loc.pre_place_approach = pre_place_approach;

        // Retreat
        moveit_msgs::GripperTranslation post_place_retreat;
        post_place_retreat.direction.header.stamp = ros::Time::now();
        post_place_retreat.desired_distance = grasp_data_.approach_retreat_desired_dist_; // The distance the origin of a robot link needs to travel
        post_place_retreat.min_distance = grasp_data_.approach_retreat_min_dist_; // half of the desired? Untested.
        post_place_retreat.direction.header.frame_id = grasp_data_.base_link_;
        post_place_retreat.direction.vector.x = 0;
        post_place_retreat.direction.vector.y = 0;
        post_place_retreat.direction.vector.z = 1; // Retreat direction (pos z axis)
        place_loc.post_place_retreat = post_place_retreat;

        // Post place posture - use same as pre-grasp posture (the OPEN command)
        place_loc.post_place_posture = grasp_data_.pre_grasp_posture_;

        place_locations.push_back(place_loc);
    }

    // Prevent collision with table
    //move_group_->setSupportSurfaceName(SUPPORT_SURFACE3_NAME);

    move_group_->setPlannerId("RRTConnectkConfigDefault");

    return move_group_->place(block_name, place_locations);
}

bool promptUser()
{
    // Make sure ROS is still with us
    if( !ros::ok() )
        return false;

    if( auto_reset_ )
    {
        ROS_INFO_STREAM_NAMED("pick_place","Auto-retrying in " << auto_reset_sec_ << " seconds");
        ros::Duration(auto_reset_sec_).sleep();
    }
    else
    {
        ROS_INFO_STREAM_NAMED("pick_place","Retry? (y/n)");
        char input; // used for prompting yes/no
        std::cin >> input;
        if( input == 'n' )
            return false;
    }
    return true;
}

void startRoutine(const std_msgs::Empty& m)
{
    double block_y = 0.1;
    double block_x = 0.35;
    MetaBlock end_block = createStartBlock(block_x, block_y, "Block1");
    end_block.goal_pose = end_block.start_pose;
    end_block.goal_pose.position.y += 0.2;
    visual_tools_->setMuted(false);
    resetBlock(end_block);

    ROS_INFO_STREAM_NAMED("pick_place","Picking '" << end_block.name << "'");
    visual_tools_->publishBlock( end_block.start_pose,
                                 moveit_visual_tools::BLUE,
                                 BLOCK_SIZE);
    while(ros::ok())
    {
        if( !pick(end_block.start_pose, end_block.name) )
        {
            ROS_ERROR_STREAM_NAMED("pick_place","Pick failed.");
            if( !promptUser() )
                break;
            resetBlock(end_block);
        }
        else
        {
            ROS_INFO_STREAM_NAMED("pick_place","Done with pick ---------------------------");
            break;
        }
    }
          
    ROS_INFO_STREAM_NAMED("pick_place","Placing '" << end_block.name << "'");
    visual_tools_->publishBlock( end_block.goal_pose,
                                 moveit_visual_tools::BLUE, BLOCK_SIZE);
    while(ros::ok())
    {
        if( !place(end_block.goal_pose, end_block.name) )
        {
            ROS_ERROR_STREAM_NAMED("pick_place","Place failed.");
            if( !promptUser() )
                break;
        }
        else
        {
            ROS_INFO_STREAM_NAMED("pick_place","Done with place ----------------------------");
            break;
        }
    }
    
    std_msgs::Empty myMsg;    
    pub_arm_done.publish(myMsg);

    ROS_INFO_STREAM_NAMED("pick_place","Finish. ----------------------------");    
}

int main(int argc, char **argv)
{
    ROS_INFO_STREAM_NAMED("temp","Starting Clam Block Pick Place");

    ros::init (argc, argv, "clam_pick_place");
    ros::AsyncSpinner spinner(1);
    spinner.start();

    ROS_INFO_STREAM_NAMED("moveit_blocks","Starting MoveIt Blocks");

    ros::NodeHandle nh_("~");

    // Get arm info from param server
    nh_.param("ee_group_name", ee_group_name_, std::string("unknown"));
    nh_.param("planning_group_name", planning_group_name_, std::string("unknown"));

    ROS_INFO_STREAM_NAMED("moveit_blocks","End Effector: " << ee_group_name_);
    ROS_INFO_STREAM_NAMED("moveit_blocks","Planning Group: " << planning_group_name_);
      
    // Create MoveGroup for one of the planning groups
    move_group_.reset(new move_group_interface::MoveGroup(planning_group_name_));
    move_group_->setPlanningTime(30.0);

    // Load grasp generator
    if (!grasp_data_.loadRobotGraspData(nh_, ee_group_name_))
        ros::shutdown();

    // Load the Robot Viz Tools for publishing to rviz
    visual_tools_.reset(new moveit_visual_tools::VisualTools( grasp_data_.base_link_));
    visual_tools_->setFloorToBaseHeight(-0.9);
    visual_tools_->loadEEMarker(grasp_data_.ee_group_, planning_group_name_);

    simple_grasps_.reset(new moveit_simple_grasps::SimpleGrasps(visual_tools_));

    // Let everything load
    ros::Duration(1.0).sleep();

    sub_turtle_done_ = nh_.subscribe("/turtle/done", 1, startRoutine);
    pub_arm_done = nh_.advertise<std_msgs::Empty>("/arm/done", 1);

    ros::spin();
    ros::shutdown();

    return 0;

}
