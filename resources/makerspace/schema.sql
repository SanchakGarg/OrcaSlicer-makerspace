-- =============================================================================
-- Makerspace OrcaSlicer Sync — Supabase Schema
-- Run this in your Supabase SQL editor (project → SQL editor → New query)
-- =============================================================================

-- ─── Extensions ──────────────────────────────────────────────────────────────
create extension if not exists "uuid-ossp";

-- ─── User roles ──────────────────────────────────────────────────────────────
-- 'member' : can sync their own presets
-- 'admin'  : can also manage shared profiles and view all users

create table if not exists makerspace_user_roles (
  user_id    uuid primary key references auth.users on delete cascade,
  role       text not null default 'member' check (role in ('member', 'admin')),
  created_at timestamptz default now()
);

-- Automatically create a 'member' role entry on first sign-up
create or replace function public.handle_new_user()
returns trigger language plpgsql security definer as $$
begin
  insert into public.makerspace_user_roles (user_id, role)
  values (new.id, 'member')
  on conflict do nothing;
  return new;
end;
$$;

drop trigger if exists on_auth_user_created on auth.users;
create trigger on_auth_user_created
  after insert on auth.users
  for each row execute procedure public.handle_new_user();

-- ─── Personal profiles ────────────────────────────────────────────────────────
-- Stores each user's own printer / filament / process presets.
-- The content column holds the full preset JSON as it appears on disk.

create table if not exists makerspace_profiles (
  id           uuid primary key default uuid_generate_v4(),
  user_id      uuid not null references auth.users on delete cascade,
  profile_type text not null check (profile_type in ('machine', 'filament', 'process')),
  name         text not null,
  vendor       text not null default '',
  content      jsonb not null default '{}',
  created_at   timestamptz default now(),
  updated_at   timestamptz default now(),
  unique (user_id, profile_type, name)
);

-- Keep updated_at fresh automatically
create or replace function public.set_updated_at()
returns trigger language plpgsql as $$
begin
  new.updated_at = now();
  return new;
end;
$$;

drop trigger if exists profiles_updated_at on makerspace_profiles;
create trigger profiles_updated_at
  before update on makerspace_profiles
  for each row execute procedure public.set_updated_at();

-- RLS — personal profiles
alter table makerspace_profiles enable row level security;

create policy "members can select own profiles"
  on makerspace_profiles for select
  using (auth.uid() = user_id);

create policy "admins can select all profiles"
  on makerspace_profiles for select
  using (
    exists (
      select 1 from makerspace_user_roles
      where user_id = auth.uid() and role = 'admin'
    )
  );

create policy "members can insert own profiles"
  on makerspace_profiles for insert
  with check (auth.uid() = user_id);

create policy "members can update own profiles"
  on makerspace_profiles for update
  using (auth.uid() = user_id);

create policy "admins can update any profile"
  on makerspace_profiles for update
  using (
    exists (
      select 1 from makerspace_user_roles
      where user_id = auth.uid() and role = 'admin'
    )
  );

create policy "members can delete own profiles"
  on makerspace_profiles for delete
  using (auth.uid() = user_id);

-- ─── Shared / org profiles ────────────────────────────────────────────────────
-- Admins upload printer / filament configs here; all members can pull them.

create table if not exists makerspace_shared_profiles (
  id           uuid primary key default uuid_generate_v4(),
  created_by   uuid not null references auth.users,
  profile_type text not null check (profile_type in ('machine', 'filament', 'process')),
  name         text not null,
  vendor       text not null default '',
  content      jsonb not null default '{}',
  is_public    boolean not null default true,
  created_at   timestamptz default now(),
  updated_at   timestamptz default now(),
  unique (profile_type, name)
);

drop trigger if exists shared_profiles_updated_at on makerspace_shared_profiles;
create trigger shared_profiles_updated_at
  before update on makerspace_shared_profiles
  for each row execute procedure public.set_updated_at();

-- RLS — shared profiles
alter table makerspace_shared_profiles enable row level security;

create policy "everyone can read public shared profiles"
  on makerspace_shared_profiles for select
  using (is_public = true);

create policy "admins can manage shared profiles"
  on makerspace_shared_profiles for all
  using (
    exists (
      select 1 from makerspace_user_roles
      where user_id = auth.uid() and role = 'admin'
    )
  );

-- ─── RLS on user roles table ─────────────────────────────────────────────────
alter table makerspace_user_roles enable row level security;

create policy "users can read own role"
  on makerspace_user_roles for select
  using (auth.uid() = user_id);

create policy "admins can read all roles"
  on makerspace_user_roles for select
  using (
    exists (
      select 1 from makerspace_user_roles r
      where r.user_id = auth.uid() and r.role = 'admin'
    )
  );

create policy "admins can update roles"
  on makerspace_user_roles for update
  using (
    exists (
      select 1 from makerspace_user_roles r
      where r.user_id = auth.uid() and r.role = 'admin'
    )
  );

-- ─── Convenience view ────────────────────────────────────────────────────────
-- Admins use this to see all users with their email and role.

create or replace view makerspace_users_view as
  select
    u.id,
    u.email,
    u.created_at,
    coalesce(r.role, 'member') as role
  from auth.users u
  left join makerspace_user_roles r on r.user_id = u.id;

-- Only admins can query this view (managed via Supabase dashboard permissions).

-- ─── Initial admin setup ─────────────────────────────────────────────────────
-- After running this schema, promote your first admin by running:
--
--   insert into makerspace_user_roles (user_id, role)
--   values ('<your-user-uuid>', 'admin')
--   on conflict (user_id) do update set role = 'admin';
--
-- You can find your user UUID in Authentication → Users in the Supabase dashboard.
