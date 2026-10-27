#include "scene/lighting_geometry.hpp"
#include "compositor/compositor.hpp"

namespace glowbox {
namespace {
// OBS may leave the first box at identity until its input size has been seen.
static void current_box(obs_sceneitem_t *item,matrix4 &box) {
    obs_sceneitem_force_update_transform(item);obs_sceneitem_get_box_transform(item,&box);
    vec2 scale;obs_sceneitem_get_box_scale(item,&scale);
    if(scale.x==0 && scale.y==0) {
        auto *source=obs_sceneitem_get_source(item);obs_sceneitem_crop crop{};obs_sceneitem_get_crop(item,&crop);
        const float w=std::max(0.f,(float)obs_source_get_width(source)-crop.left-crop.right);
        const float h=std::max(0.f,(float)obs_source_get_height(source)-crop.top-crop.bottom);
        matrix4 size,draw;matrix4_identity(&size);matrix4_scale3f(&size,&size,w,h,1);
        obs_sceneitem_get_draw_transform(item,&draw);matrix4_mul(&box,&size,&draw);
    }
}
struct LightSearch {
    const std::string *name;
    matrix4 transform = {};
    vec2 centre = {};
    ReactiveGeometry placement = {};
    unsigned matches = 0, depth = 0;
    obs_data_t *roles = nullptr;
    bool parent_subject = false;
    float subject_weight = 0;
};
bool find_light_item(obs_scene_t *,obs_sceneitem_t *item,void *data) {
    auto &search=*static_cast<LightSearch *>(data);
    if(!obs_sceneitem_visible(item))return true;
    auto *source=obs_sceneitem_get_source(item);
    matrix4 box,world;
    current_box(item,box);matrix4_mul(&world,&box,&search.transform);
    const auto &name=*search.name;
    const bool subject = search.depth ? search.parent_subject :
        (search.roles && obs_data_get_bool(search.roles,subject_setting_key(item).c_str()));
    if(name==obs_source_get_name(source) ||
       (name.compare(0,5,"uuid:")==0 && name.compare(5,std::string::npos,obs_source_get_uuid(source))==0)) {
        vec3 centre;vec3_set(&centre,.5f,.5f,0);vec3_transform(&centre,&centre,&world);
        search.centre={centre.x,centre.y};search.subject_weight=subject?1.f:0.f;
        search.placement.subject_plane_weight=search.subject_weight;++search.matches;
        vec3 a,b,c;vec3_set(&a,0,0,0);vec3_set(&b,1,0,0);vec3_set(&c,0,1,0);
        vec3_transform(&a,&a,&world);vec3_transform(&b,&b,&world);vec3_transform(&c,&c,&world);
        search.placement.origin={a.x,a.y};search.placement.axis_x={b.x-a.x,b.y-a.y};
        search.placement.axis_y={c.x-a.x,c.y-a.y};
        obs_sceneitem_crop crop{};obs_sceneitem_get_crop(item,&crop);
        const float w=(float)obs_source_get_width(source),h=(float)obs_source_get_height(source);
        if(w>0 && h>0)search.placement.crop={crop.left/w,crop.top/h,
            std::max(0.f,w-crop.left-crop.right)/w,std::max(0.f,h-crop.top-crop.bottom)/h};
    }
    auto *child=obs_sceneitem_is_group(item)?obs_sceneitem_group_get_scene(item):obs_scene_from_source(source);
    if(child && search.depth<16) {
        matrix4 draw;obs_sceneitem_get_draw_transform(item,&draw);
        LightSearch nested=search;matrix4_mul(&nested.transform,&draw,&search.transform);
        nested.matches=0;nested.parent_subject=subject;++nested.depth;
        obs_scene_enum_items(child,find_light_item,&nested);
        if(nested.matches){search.centre=nested.centre;search.placement=nested.placement;search.subject_weight=nested.subject_weight;search.matches+=nested.matches;}
    }
    return true;
}
struct SubjectBounds {
    obs_data_t *roles;
    float left=INFINITY,top=INFINITY,right=-INFINITY,bottom=-INFINITY;
    obs_source_t *exclude=nullptr;
};
bool subject_bounds(obs_scene_t *,obs_sceneitem_t *item,void *data) {
    auto &bounds=*static_cast<SubjectBounds *>(data);
    if(!obs_sceneitem_visible(item) || obs_sceneitem_get_source(item)==bounds.exclude ||
       !obs_data_get_bool(bounds.roles,subject_setting_key(item).c_str()) ||
       source_protection(obs_sceneitem_get_source(item))!=SourceProtection::None)return true;
    matrix4 box;current_box(item,box);
    for(float x:{0.f,1.f})for(float y:{0.f,1.f}) {
        vec3 point;vec3_set(&point,x,y,0);vec3_transform(&point,&point,&box);
        bounds.left=std::min(bounds.left,point.x);bounds.right=std::max(bounds.right,point.x);
        bounds.top=std::min(bounds.top,point.y);bounds.bottom=std::max(bounds.bottom,point.y);
    }
    return true;
}
}
vec2 subject_centre(const Compositor *f, obs_scene_t *scene, obs_data_t *roles) {
    SubjectBounds bounds{roles ? roles : f->layer_settings};
    if (scene) obs_scene_enum_items(scene,subject_bounds,&bounds);
    return std::isfinite(bounds.left) ? vec2{(bounds.left+bounds.right)*.5f,(bounds.top+bounds.bottom)*.5f} :
        vec2{f->width*.5f,f->height*.5f};
}
RimLightGeometry rim_light_geometry(const Compositor *f,obs_scene_t *scene,obs_data_t *roles) {
    RimLightGeometry result;
    if(!scene || !f->rim_follow_source || (f->rim_type!=0 && f->rim_type!=2))return result;
    LightSearch search{&f->reactive_source_id};search.roles=roles?roles:f->layer_settings;matrix4_identity(&search.transform);
    if (f->rim_follow_light == 2) {
        search.matches = 1;
        const float width=(float)obs_source_get_width(obs_scene_get_source(scene));
        const float height=(float)obs_source_get_height(obs_scene_get_source(scene));
        const auto emitter=spotlight_emitter_position(f,width/std::max(height,1.f));
        search.centre = {emitter.x*width,emitter.y*height};
    } else if (!f->reactive_source_id.empty()) obs_scene_enum_items(scene,find_light_item,&search);
    SubjectBounds bounds{roles?roles:f->layer_settings};obs_scene_enum_items(scene,subject_bounds,&bounds);
    // Multiple visible placements have no unique source position. Preserve manual control.
    if(search.matches!=1 || !std::isfinite(bounds.left) || !std::isfinite(bounds.right) ||
       !std::isfinite(bounds.top) || !std::isfinite(bounds.bottom))return result;
    result.light=search.centre;result.light_subject_weight=search.subject_weight;
    result.subject={(bounds.left+bounds.right)*.5f,(bounds.top+bounds.bottom)*.5f};
    result.valid=true;return result;
}
ReactiveGeometry reactive_geometry(const Compositor *f,obs_scene_t *scene,obs_data_t *roles,bool reference_view) {
    ReactiveGeometry result;
    if(!scene || f->reactive_source_id.empty())return result;
    const float h=(float)obs_source_get_height(obs_scene_get_source(scene));
    const float w=(float)obs_source_get_width(obs_scene_get_source(scene));
    if(w<=0 || h<=0)return result;
    LightSearch search{&f->reactive_source_id};search.roles=roles?roles:f->layer_settings;matrix4_identity(&search.transform);
    obs_scene_enum_items(scene,find_light_item,&search);
    if(search.matches!=1)return result; // No unique placement: retain global lighting.
    result=search.placement;
    const float emitter=std::max(std::hypot(result.axis_x.x,result.axis_x.y),
                                 std::hypot(result.axis_y.x,result.axis_y.y));
    if(emitter<1 || result.crop.z<=0 || result.crop.w<=0)return result;
    auto *light=acquire_source_reference(f->reactive_source_id.c_str());
    SubjectBounds receiver{roles?roles:f->layer_settings};receiver.exclude=light;
    obs_scene_enum_items(scene,subject_bounds,&receiver);
    if(light)obs_source_release(light);
    const float receiver_size=std::isfinite(receiver.top)?
        std::max(receiver.right-receiver.left,receiver.bottom-receiver.top):h;
    // Keep a minimum distance and scale it with the emitter and receiver.

    result.receiver_extent=receiver_size/h;
    result.depth=std::max({.06f,.12f*emitter/h,.25f*result.receiver_extent});
    result.origin.x/=h;result.origin.y/=h;result.axis_x.x/=h;result.axis_x.y/=h;
    result.axis_y.x/=h;result.axis_y.y/=h;result.aspect=w/h;result.valid=true;
    if (!reference_view) project_reactive_geometry(f,result);
    return result;
}
void project_reactive_geometry(const Compositor *f,ReactiveGeometry &geometry) {
    if (!geometry.valid || !scene_view_active(f->view)) return;
    const float distance=1.f+(subject_plane_distance(f->subject_depth)-1.f)*geometry.subject_plane_weight;
    geometry.origin=scene_project_point(f->view,distance,1.f,geometry.origin);
}
bool rim_angle_between(const vec2 &light,const vec2 &subject,float &angle) {
    const float dx=light.x-subject.x,dy=light.y-subject.y;
    if(!std::isfinite(dx) || !std::isfinite(dy) || dx*dx+dy*dy<1.f)return false;
    angle=std::atan2(dx,-dy)*57.295779513f;return true;
}
void update_rim_direction(Compositor *f,obs_scene_t *scene) {
    if (f->shadow_amount > 0 && (!f->shadow_follow || f->lighting.spotlight_amount < 1)) {
        f->shadow_pivot = subject_centre(f,scene);
        f->shadow_pivot.x /= std::max(1.f,(float)f->width);
        f->shadow_pivot.y /= std::max(1.f,(float)f->height);
    }
    auto geometry=rim_light_geometry(f,scene);
    if (geometry.valid && scene_view_active(f->view)) {
        const float subject_distance=subject_plane_distance(f->subject_depth);
        const float light_distance=1.f+(subject_distance-1.f)*geometry.light_subject_weight;
        geometry.light=scene_project_point(f->view,light_distance,float(f->height),geometry.light);
        geometry.subject=scene_project_point(f->view,subject_distance,float(f->height),geometry.subject);
    }
    f->rim_follow_ready=geometry.valid && rim_angle_between(geometry.light,geometry.subject,f->rim_follow_angle);
}
}
