/*********************************************************************
* Software License Agreement (BSD License)
* 
*  Copyright (c) 2008, Willow Garage, Inc.
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
*   * Neither the name of the Willow Garage nor the names of its
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

/* Author: Ioan Sucan */

#include "ompl/control/planners/rrt/RRT.h"
#include "ompl/base/GoalSampleableRegion.h"
#include "ompl/datastructures/NearestNeighborsSqrtApprox.h"
#include <limits>

void ompl::control::RRT::setup(void)
{
    base::Planner::setup();	    
    if (!nn_)
	nn_.reset(new NearestNeighborsSqrtApprox<Motion*>());
    nn_->setDistanceFunction(boost::bind(&RRT::distanceFunction, this, _1, _2));
}

void ompl::control::RRT::clear(void)
{
    Planner::clear();
    sampler_.reset();
    controlSampler_.reset();
    freeMemory();
    nn_->clear();
}

bool ompl::control::RRT::solve(const base::PlannerTerminationCondition &ptc)
{
    pis_.checkValidity();
    base::Goal                   *goal = pdef_->getGoal().get(); 
    base::GoalSampleableRegion *goal_s = dynamic_cast<base::GoalSampleableRegion*>(goal);

    if (!goal)
    {
	msg_.error("Goal undefined");
	return false;
    }

    while (const base::State *st = pis_.nextStart())
    { 
	Motion *motion = new Motion(siC_);
	si_->copyState(motion->state, st);
	siC_->nullControl(motion->control);
	nn_->add(motion);
    }
    
    if (nn_->size() == 0)
    {
	msg_.error("There are no valid initial states!");
	return false;	
    }    

    if (!sampler_)
	sampler_ = si_->allocManifoldStateSampler();
    if (!controlSampler_)
	controlSampler_ = siC_->allocControlSampler();

    msg_.inform("Starting with %u states", nn_->size());
    
    Motion *solution  = NULL;
    Motion *approxsol = NULL;
    double  approxdif = std::numeric_limits<double>::infinity();
    
    Motion      *rmotion = new Motion(siC_);
    base::State  *rstate = rmotion->state;
    Control       *rctrl = rmotion->control;
    base::State  *xstate = si_->allocState();
    
    while (ptc() == false)
    {	
	/* sample random state (with goal biasing) */
	if (goal_s && rng_.uniform01() < goalBias_ && goal_s->canSample())
	    goal_s->sampleGoal(rstate);
	else
	    sampler_->sampleUniform(rstate);
	
	/* find closest state in the tree */
	Motion *nmotion = nn_->nearest(rmotion);
	
	/* sample a random control */
	controlSampler_->sampleNext(rctrl, nmotion->control, nmotion->state);
	unsigned int cd = controlSampler_->sampleStepCount(siC_->getMinControlDuration(), siC_->getMaxControlDuration());
	cd = siC_->propagateWhileValid(nmotion->state, rctrl, cd, xstate);

	if (cd >= siC_->getMinControlDuration())
	{
	    /* create a motion */
	    Motion *motion = new Motion(siC_);
	    si_->copyState(motion->state, xstate);
	    siC_->copyControl(motion->control, rctrl);
	    motion->steps = cd;
	    motion->parent = nmotion;
	    
	    nn_->add(motion);
	    double dist = 0.0;
	    bool solved = goal->isSatisfied(motion->state, &dist);
	    if (solved)
	    {
		approxdif = dist;
		solution = motion;
		break;
	    }
	    if (dist < approxdif)
	    {
		approxdif = dist;
		approxsol = motion;
	    }
	}
    }
    
    bool approximate = false;
    if (solution == NULL)
    {
	solution = approxsol;
	approximate = true;
    }
    
    if (solution != NULL)
    {
	/* construct the solution path */
	std::vector<Motion*> mpath;
	while (solution != NULL)
	{
	    mpath.push_back(solution);
	    solution = solution->parent;
	}

	/* set the solution path */
	PathControl *path = new PathControl(si_);
   	for (int i = mpath.size() - 1 ; i >= 0 ; --i)
	{   
	    path->states.push_back(si_->cloneState(mpath[i]->state));
	    if (mpath[i]->parent)
	    {
		path->controls.push_back(siC_->cloneControl(mpath[i]->control));
		path->controlDurations.push_back(mpath[i]->steps * siC_->getPropagationStepSize());
	    }
	}
	goal->setDifference(approxdif);
	goal->setSolutionPath(base::PathPtr(path), approximate);

	if (approximate)
	    msg_.warn("Found approximate solution");
    }
    
    if (rmotion->state)
	si_->freeState(rmotion->state);
    if (rmotion->control)
	siC_->freeControl(rmotion->control);
    delete rmotion;
    si_->freeState(xstate);
    
    msg_.inform("Created %u states", nn_->size());
    
    return goal->isAchieved();
}

void ompl::control::RRT::getPlannerData(base::PlannerData &data) const
{
    Planner::getPlannerData(data);

    std::vector<Motion*> motions;
    if (nn_)
	nn_->list(motions);

    for (unsigned int i = 0 ; i < motions.size() ; ++i)
	data.recordEdge(motions[i]->parent ? motions[i]->parent->state : NULL, motions[i]->state);
}