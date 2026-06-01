2. **Your job-**is to inspect the existing VR engine and OpenXR locomotion code and fix the parts responsible for:  
   1. **Reading the left joystick movement input**  
      2. **Reading the latest HMD/camera pose**  
         3. **Reading the latest left controller pose**  
         4. **Choosing the correct locomotion mode**  
         5. **Converting joystick input into movement direction**  
         6. **Applying movement to the player origin/root**  
         7. **Handling OpenXR reference spaces correctly**  
      3. **Head-Relative Locomotion required behavior:**   
         1. When Head-Relative Locomotion is selected, joystick movement must be based on the current HMD/camera yaw direction.  
         2. If I look at a wall and push the left joystick forward, I should move toward that wall.  
         3. If I turn my head 180 degrees and push forward, I should move in the new direction I am facing, not the original world direction.  
         4. If I turn my head while holding the joystick forward, the movement direction should update based on the latest HMD/camera direction.  
         5. Head-Relative Locomotion must use the current HMD/camera pose as the movement reference, not a fixed world-space direction.  
      4. **Controller-Relative Locomotion required behavior:**   
         1. When Controller-Relative Locomotion is selected, joystick movement must be based on the current left controller orientation, not the HMD/camera direction.  
         2. If I rotate or point the left controller in a different direction, the movement basis should update from the latest left controller pose.  
         3. The player’s head direction should not control Controller-Relative Locomotion.  
         4. Controller-Relative Locomotion must use the current left controller pose as the movement reference, not a fixed world-space direction.  
      5. Important implementation requirements:  
         1. Remove the fixed world-space movement basis.  
            1. Find where the locomotion system currently uses a fixed world forward, global forward, default forward direction, cached startup direction, or any direction that does not update when the HMD/controller rotates.  
            2. Replace that logic with pose-relative movement direction logic based on the selected locomotion mode.  
         2. Use the correct tracked pose source.   
            1. For Head-Relative Locomotion, use the latest HMD/camera pose.  
            2. For Controller-Relative Locomotion, use the latest left controller pose.  
            3. Do not mix the HMD pose and controller pose.  
            4. Do not use the player’s original spawn rotation as the ongoing movement basis.  
         3. Use the latest tracking data every frame.  
            1. The movement direction must be calculated from the most recent HMD/controller pose available for the current frame.  
            2. Do not use stale cached orientation data unless that data is refreshed every frame before locomotion is calculated.  
         4. Keep movement horizontal.  
            1. For walking/sliding locomotion, ignore vertical pitch/tilt from the HMD or controller.  
            2. The user should not move upward because they look up, and they should not move downward because they look down.  
            3. Movement should stay parallel to the virtual ground plane while the engine’s normal systems handle vertical motion separately.  
         5. Apply movement to the correct object.  
            1. The final movement should be applied to the VR player origin, rig root, or equivalent player root transform.  
            2. Do not directly overwrite or manually move the HMD/camera pose, because the HMD/camera pose should remain driven by OpenXR tracking.  
         6. Verify OpenXR reference space handling.  
            1. Check how this engine uses OpenXR reference spaces such as LOCAL, LOCAL\_FLOOR, STAGE, VIEW, grip pose, and aim pose.  
            2. Make sure the HMD/camera pose, left controller pose, and player root movement are all being interpreted in compatible coordinate spaces.  
            3. If the current code mixes coordinate spaces incorrectly, fix the conversion so the tracked pose direction and movement application are in the same usable world/player space.  
         7. Fix the locomotion mode switch.  
            1. Check the logic that switches between Head-Relative and Controller-Relative Locomotion.  
            2. Make sure the selected mode actually changes which tracked pose is used as the movement reference.  
            3. If both modes currently behave the same, fix the branching or mode-selection logic.  
      6. Resources (Look at this documentation I provided below and find your own too to reach my goal)  
         1. Meta Locomotion Types: [https://developers.meta.com/horizon/design/locomotion-types/](https://developers.meta.com/horizon/design/locomotion-types/?utm_source=chatgpt.com)   
         2. Meta Sliding / Artificial Locomotion: [https://developers.meta.com/horizon/design/locomotion-types/\#artificial-locomotion-types](https://developers.meta.com/horizon/design/locomotion-types/#artificial-locomotion-types)  
         3. Meta Locomotion Input Maps: [https://developers.meta.com/horizon/design/locomotion-input-maps/](https://developers.meta.com/horizon/design/locomotion-input-maps/)  
         4. Meta Slide Interaction / Slide Locomotion: [https://developers.meta.com/horizon/documentation/unity/unity-isdk-slide-interaction/](https://developers.meta.com/horizon/documentation/unity/unity-isdk-slide-interaction/?utm_source=chatgpt.com)  
         5. Meta IWSDK Locomotion Slide: [https://developers.meta.com/horizon/documentation/web/iwsdk-concept-locomotion-slide/](https://developers.meta.com/horizon/documentation/web/iwsdk-concept-locomotion-slide/?utm_source=chatgpt.com)  
         6. Unity XR Interaction Toolkit Continuous Move Provider: [https://docs.unity3d.com/Packages/com.unity.xr.interaction.toolkit@3.0/manual/continuous-move-provider.html](https://docs.unity3d.com/Packages/com.unity.xr.interaction.toolkit@3.0/manual/continuous-move-provider.html)  
      7. Final goal:Update my custom VR engine and OpenXR locomotion implementation so:  
         1. Head-Relative Locomotion uses the latest HMD/camera yaw direction.  
         2. Controller-Relative Locomotion uses the latest left controller yaw direction.  
         3. Joystick movement is converted into movement using the correct tracked pose reference.  
         4. Movement is no longer based on a fixed world-space direction.  
         5. Looking up/down or tilting the controller does not cause vertical walking movement.  
         6. Movement is applied to the player origin/root, not directly to the HMD/camera.  
         7. The 180-degree turning bug is fully fixed as well as if I turn any direction and then move the locomotion works perfectly like it would for a VR Game.