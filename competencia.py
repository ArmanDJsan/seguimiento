import numpy as np
import matplotlib.pyplot as plt
import matplotlib.animation as animation

# Create a figure and axis
fig, ax = plt.subplots()
ax.set_xlim(0, 42)
ax.set_ylim(0, 16)

# Initial positions of the circles
circle1 = plt.Circle((0, 5), 0.5, color='yellow')
circle2 = plt.Circle((0, 3), 0.5, color='blue')
circle3 = plt.Circle((0, 8 ), 0.5, color='orange')
circle4 = plt.Circle((0, 11), 0.5, color='green') 
ax.add_patch(circle1)
ax.add_patch(circle2)
ax.add_patch(circle3)
ax.add_patch(circle4)
# Draw the start and finish lines
ax.axvline(x=5, color='green', linestyle='--', linewidth=2)  # Start line
ax.axvline(x=40, color='red', linestyle='--', linewidth=2)  # Finish line

# Initial velocities of the circles
velocity1 = np.random.uniform(0.1, 0.5)
velocity2 = np.random.uniform(0.1, 0.5)
velocity3 = np.random.uniform(0.1, 0.5)
velocity4 = np.random.uniform(0.1, 0.5)
# Function to update the position of the circles
def update(frame):
    global velocity1, velocity2, velocity3, velocity4
    new_x1 = circle1.center[0] + velocity1
    new_x2 = circle2.center[0] + velocity2
    new_x3 = circle3.center[0] + velocity3
    new_x4 = circle4.center[0] + velocity4
    
    # Update the positions of the circles
    circle1.set_center((new_x1, 5))
    circle2.set_center((new_x2, 3))
    circle3.set_center((new_x3, 8))
    circle4.set_center((new_x4, 11))
    
    # Randomly adjust the velocities
    velocity1 += np.random.uniform(-0.05, 0.05)
    velocity2 += np.random.uniform(-0.05, 0.05)
    velocity3 += np.random.uniform(-0.05, 0.05)
    velocity4 += np.random.uniform(-0.05, 0.05)
    
    # Ensure the velocities stay within a reasonable range
    velocity1 = np.clip(velocity1, 0.1, 0.5)
    velocity2 = np.clip(velocity2, 0.1, 0.5)
    velocity3 = np.clip(velocity3, 0.1, 0.5)
    velocity4 = np.clip(velocity4, 0.1, 0.5)
    
    return circle1, circle2, circle3, circle4

# Create an animation
ani = animation.FuncAnimation(fig, update, frames=np.arange(0, 220), blit=True)

# Save the animation as a video file
ani.save('circles.mp4', writer='ffmpeg', fps=30)

plt.show()